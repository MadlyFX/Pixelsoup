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
#include <vector>

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

struct TakePlate final {
    std::size_t slot{};
    std::string source;
    core::u64 asset{};
};

// A plateset take as plateset authoring writes it: an ordinary clip of its
// front plate that also names the plate for every slot it fills.
[[nodiscard]] doc::Node plateset_take(core::u64 id, core::u64 plateset,
                                      const std::vector<TakePlate>& plates) {
    doc::Node node = placement(id, scene::schema::media_type,
                               plates.front().source, plates.front().asset);
    node.properties.emplace(
        std::string{scene::schema::plateset_asset},
        doc::PropertyValue{static_cast<std::int64_t>(plateset)});
    for (const TakePlate& plate : plates) {
        node.properties.emplace(
            scene::schema::plate_property(plate.slot,
                                          scene::schema::plate_source_suffix),
            doc::PropertyValue{plate.source});
        node.properties.emplace(
            scene::schema::plate_property(plate.slot,
                                          scene::schema::plate_asset_suffix),
            doc::PropertyValue{static_cast<std::int64_t>(plate.asset)});
        node.properties.emplace(
            scene::schema::plate_property(plate.slot,
                                          scene::schema::plate_count_suffix),
            doc::PropertyValue{std::int64_t{6'420}});
        node.properties.emplace(
            scene::schema::plate_property(plate.slot,
                                          scene::schema::plate_offset_suffix),
            doc::PropertyValue{std::int64_t{0}});
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

void every_plate_of_a_plateset_take_has_to_be_opened() {
    // Front plate 0 is also the clip's own source; the take fills three more
    // slots, out of slot order, each from its own Library record.
    const std::vector<TakePlate> plates{
        {0U, src("plate-front.mov"), 31U},
        {4U, src("plate-back.mov"), 35U},
        {2U, src("plate-right.mov"), 33U},
        {8U, src("plate-ceiling.mov"), 39U},
    };
    doc::Root root;
    for (const TakePlate& plate : plates) {
        root = root.set(asset_node(plate.asset, assets::schema::media_type,
                                   plate.source));
    }
    root = root.set(plateset_take(50U, 77U, plates));

    const auto manifest = manifest_of(root);
    // Four files, not five: the front plate named twice is one requirement.
    PROJECT_ASSET_CHECK(manifest.assets.size() == 4U);
    for (const TakePlate& plate : plates) {
        const auto found = std::find_if(
            manifest.assets.begin(), manifest.assets.end(),
            [&](const app::RequiredProjectAsset& required) {
                return required.source == plate.source;
            });
        PROJECT_ASSET_CHECK(found != manifest.assets.end());
        PROJECT_ASSET_CHECK(found->kind == assets::AssetKind::Media);
        PROJECT_ASSET_CHECK(found->asset == doc::NodeId{plate.asset});
    }
    PROJECT_ASSET_CHECK(manifest.kind_mismatches == 0U);

    // Repointing one non-front plate is a different set of files to check.
    doc::Node repointed = *root.get(doc::NodeId{50U});
    repointed.properties.insert_or_assign(
        scene::schema::plate_property(4U, scene::schema::plate_source_suffix),
        doc::PropertyValue{src("plate-back-v2.mov")});
    const auto changed = manifest_of(root.set(repointed));
    PROJECT_ASSET_CHECK(changed.digest != manifest.digest);
    PROJECT_ASSET_CHECK(requires_source(changed, src("plate-back-v2.mov")));
    PROJECT_ASSET_CHECK(!requires_source(changed, src("plate-back.mov")));
}

void plate_properties_count_only_on_a_clip_from_a_plateset() {
    // The scene model reads plates only from a clip carrying plateset_asset;
    // the manifest must agree, or it asks nodes for files nothing draws.
    doc::Root root;
    doc::Node clip =
        placement(20U, scene::schema::media_type, src("clip.mp4"));
    clip.properties.emplace(
        scene::schema::plate_property(3U, scene::schema::plate_source_suffix),
        doc::PropertyValue{src("stray-plate.mov")});
    root = root.set(clip);

    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(manifest.assets[0U].source == src("clip.mp4"));
}

void a_plate_revision_is_judged_by_size_and_time_alone() {
    // Readiness has to stay quick on multi-gigabyte plates: the revision is
    // the file's size and last-write time, and its bytes are never read.
    TemporaryDirectory temporary;
    const fs::path front = temporary.path() / "front.mov";
    const fs::path back = temporary.path() / "back.mov";
    write(front, "front-plate-bytes");
    write(back, "back-plate-bytes");
    const std::string front_source =
        assets::asset_source_from_path(front);
    const std::string back_source =
        assets::asset_source_from_path(back);
    const auto front_revision = assets::source_fingerprint(front_source);
    const auto back_revision = assets::source_fingerprint(back_source);
    PROJECT_ASSET_CHECK(front_revision);
    PROJECT_ASSET_CHECK(back_revision);

    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        31U, assets::schema::media_type, front_source,
        front_revision.value()));
    root = root.set(fingerprinted_asset_node(
        35U, assets::schema::media_type, back_source, back_revision.value()));
    root = root.set(plateset_take(
        50U, 77U, {{0U, front_source, 31U}, {4U, back_source, 35U}}));
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);

    // Same length, same timestamp, different bytes: still the same revision.
    const auto stamp = fs::last_write_time(back);
    write(back, "BACK-PLATE-BYTES");
    fs::last_write_time(back, stamp);
    const auto same = app::check_project_assets(manifest);
    PROJECT_ASSET_CHECK(same.ready());
    PROJECT_ASSET_CHECK(same.assets.size() == 2U);

    // A new timestamp is a new revision, reported against that plate only.
    fs::last_write_time(back, stamp + std::chrono::seconds{90});
    const auto touched = app::check_project_assets(manifest);
    PROJECT_ASSET_CHECK(!touched.ready());
    PROJECT_ASSET_CHECK(touched.mismatched == 1U);
    const auto stale = std::find_if(
        touched.assets.begin(), touched.assets.end(),
        [&](const app::ProjectAssetPresence& presence) {
            return presence.source == back_source;
        });
    PROJECT_ASSET_CHECK(stale != touched.assets.end());
    PROJECT_ASSET_CHECK(stale->state ==
                        app::ProjectAssetPresence::State::Sha256Mismatch);
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

// RESCAN also asks every Library asset - used or not - whether its file is
// still the revision it recorded, and an accepted revision drops out of the
// answer without touching the disk again.
void rescan_finds_changed_library_sources_until_they_are_accepted() {
    TemporaryDirectory temporary;
    const fs::path changed_path = temporary.path() / "changed.mov";
    const fs::path steady_path = temporary.path() / "steady.mov";
    write(changed_path, "frames");
    write(steady_path, "frames");
    const std::string changed_source =
        assets::asset_source_from_path(changed_path);
    const std::string steady_source =
        assets::asset_source_from_path(steady_path);
    const auto recorded = assets::source_fingerprint(changed_source);
    const auto steady = assets::source_fingerprint(steady_source);
    PROJECT_ASSET_CHECK(recorded && steady);

    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, changed_source, recorded.value()));
    root = root.set(fingerprinted_asset_node(
        11U, assets::schema::media_type, steady_source, steady.value()));
    const auto registry_of = [](const doc::Root& document) {
        assets::AssetModel model;
        model.reset(document);
        return model.evaluate();
    };
    PROJECT_ASSET_CHECK(
        app::find_changed_library_sources(registry_of(root)).empty());

    const auto original_time = fs::last_write_time(changed_path);
    write(changed_path, "new frames");
    fs::last_write_time(changed_path,
                        original_time + std::chrono::seconds{2});
    const auto found = app::find_changed_library_sources(registry_of(root));
    PROJECT_ASSET_CHECK(found.size() == 1U);
    PROJECT_ASSET_CHECK(found[0U].asset == doc::NodeId{10U});
    PROJECT_ASSET_CHECK(found[0U].observed.byte_length == 10U);
    PROJECT_ASSET_CHECK(
        app::still_changed_library_sources(registry_of(root), found) ==
        std::vector<doc::NodeId>{doc::NodeId{10U}});

    // Accepting records what was found; the asset is no longer changed.
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, changed_source, found[0U].observed));
    PROJECT_ASSET_CHECK(
        app::still_changed_library_sources(registry_of(root), found).empty());
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

void ies_sources_participate_in_the_project_manifest() {
    doc::Root root;
    doc::Node light{doc::NodeId{40U}, std::string{scene::schema::light_type}, {}};
    light.properties.emplace(std::string{scene::schema::ies_source}, doc::PropertyValue{src("practical.ies")});
    root = root.set(light);
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(manifest.assets[0].kind == assets::AssetKind::IesProfile);
    PROJECT_ASSET_CHECK(requires_source(manifest, src("practical.ies")));
    light.properties[std::string{scene::schema::ies_source}] = doc::PropertyValue{src("replacement.ies")};
    PROJECT_ASSET_CHECK(manifest_of(root.set(light)).digest != manifest.digest);
}

void particle_packages_participate_in_the_project_manifest() {
    // A placed effect is a file every node opens, linked to its Library
    // record by asset_id like any placement.
    doc::Root root;
    doc::Node record{doc::NodeId{50U}, std::string{assets::schema::particle_effect_type}, {}};
    record.properties.emplace(std::string{assets::schema::display_name}, doc::PropertyValue{std::string{"Embers"}});
    record.properties.emplace(std::string{assets::schema::source}, doc::PropertyValue{src("embers.ajfx")});
    root = root.set(record);
    doc::Node effect{doc::NodeId{51U}, std::string{scene::schema::particle_effect_type}, {}};
    effect.properties.emplace(std::string{scene::schema::source}, doc::PropertyValue{src("embers.ajfx")});
    effect.properties.emplace(std::string{scene::schema::asset_id}, doc::PropertyValue{std::int64_t{50}});
    root = root.set(effect);
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(manifest.assets[0].kind == assets::AssetKind::ParticleEffect);
    PROJECT_ASSET_CHECK(manifest.assets[0].asset == doc::NodeId{50U});
    PROJECT_ASSET_CHECK(requires_source(manifest, src("embers.ajfx")));
    effect.properties[std::string{scene::schema::source}] = doc::PropertyValue{src("sparks.ajfx")};
    PROJECT_ASSET_CHECK(manifest_of(root.set(effect)).digest != manifest.digest);
}

void colour_luts_participate_in_the_project_manifest() {
    doc::Node wall{doc::NodeId{41U}, "output.surface", {}};
    wall.properties["camera_inverse_lut"] = src("camera.cube");
    doc::Node settings{doc::NodeId{42U}, std::string{scene::schema::settings_type}, {}};
    settings.properties["show_lut"] = src("show.cube");
    const auto root = doc::Root{}.set(wall).set(settings);
    const auto manifest = manifest_of(root);
    PROJECT_ASSET_CHECK(manifest.assets.size() == 2U);
    for (const auto& asset : manifest.assets) PROJECT_ASSET_CHECK(asset.kind == assets::AssetKind::ColorLut);
    PROJECT_ASSET_CHECK(requires_source(manifest, src("camera.cube")));
    PROJECT_ASSET_CHECK(requires_source(manifest, src("show.cube")));
    wall.properties["camera_inverse_lut"] = src("replacement.cube");
    PROJECT_ASSET_CHECK(manifest_of(root.set(wall)).digest != manifest.digest);
}

void an_image_sequence_is_present_when_its_first_frame_is() {
    TemporaryDirectory directory;
    write(directory.path() / "plate_1001.png", "frame");
    auto clip = placement(43U, scene::schema::media_type,
                          (directory.path() / "plate_####.png").string());
    clip.properties.emplace(std::string{scene::schema::sequence_start},
                            doc::PropertyValue{std::int64_t{1001}});
    const auto manifest = manifest_of(doc::Root{}.set(clip));
    PROJECT_ASSET_CHECK(manifest.assets.size() == 1U);
    PROJECT_ASSET_CHECK(requires_source(manifest, (directory.path() / "plate_1001.png").string()));
    PROJECT_ASSET_CHECK(app::check_project_assets(manifest).ready());
    PROJECT_ASSET_CHECK(app::scan_project_assets(manifest).ready());
    clip.properties[std::string{scene::schema::sequence_start}] = doc::PropertyValue{std::int64_t{1}};
    PROJECT_ASSET_CHECK(!app::scan_project_assets(manifest_of(doc::Root{}.set(clip))).ready());
}

// A show with one clip on disk and one model that is not, in `directory`.
struct HalfPresentShow final {
    doc::Root root;
    std::string present_source;
    std::string absent_source;
};

[[nodiscard]] HalfPresentShow half_present_show(const fs::path& directory) {
    const fs::path present = directory / "clip.mp4";
    write(present, "frames");
    HalfPresentShow show;
    show.present_source = assets::asset_source_from_path(present);
    show.absent_source =
        assets::asset_source_from_path(directory / "set.glb");
    show.root = show.root.set(
        asset_node(10U, assets::schema::media_type, show.present_source));
    show.root = show.root.set(placement(
        20U, scene::schema::media_type, show.present_source, 10U));
    show.root = show.root.set(
        placement(21U, scene::schema::model_type, show.absent_source));
    return show;
}

[[nodiscard]] const app::ProjectAssetPresence* presence_of(
    const app::ProjectAssetAvailability& availability,
    std::string_view source) {
    const auto found = std::find_if(
        availability.assets.begin(), availability.assets.end(),
        [source](const app::ProjectAssetPresence& presence) {
            return presence.source == source;
        });
    return found == availability.assets.end() ? nullptr : &*found;
}

// The scanner answers the question the frame loop used to ask itself, from a
// thread of its own: what it reports for a manifest is what is on the disk.
void the_scanner_reports_what_is_on_disk_for_the_manifest_asked() {
    TemporaryDirectory temporary;
    const HalfPresentShow show = half_present_show(temporary.path());
    const auto manifest = manifest_of(show.root);

    app::ProjectAssetScanner scanner;
    PROJECT_ASSET_CHECK(!scanner.take().has_value());
    const core::u64 ticket = scanner.request(manifest);
    PROJECT_ASSET_CHECK(ticket != 0U);
    scanner.drain();

    const auto scanned = scanner.take();
    PROJECT_ASSET_CHECK(scanned.has_value());
    PROJECT_ASSET_CHECK(scanned->ticket == ticket);
    PROJECT_ASSET_CHECK(scanned->depth == app::ProjectAssetScanDepth::Presence);
    PROJECT_ASSET_CHECK(scanned->availability.digest == manifest.digest);
    PROJECT_ASSET_CHECK(scanned->availability.assets.size() == 2U);
    PROJECT_ASSET_CHECK(scanned->availability.missing == 1U);
    const auto* clip =
        presence_of(scanned->availability, show.present_source);
    const auto* set = presence_of(scanned->availability, show.absent_source);
    PROJECT_ASSET_CHECK(clip != nullptr && clip->present);
    PROJECT_ASSET_CHECK(set != nullptr && !set->present);
    PROJECT_ASSET_CHECK(set->kind == assets::AssetKind::Model);
    // A missing model keeps this machine from being ready to render.
    PROJECT_ASSET_CHECK(!scanned->availability.render_ready());
    // An answer is taken once.
    PROJECT_ASSET_CHECK(!scanner.take().has_value());

    // Asked again after the file arrives - which only ever happens because
    // somebody asked - the answer is the new state of the disk.
    write(temporary.path() / "set.glb", "mesh");
    const core::u64 again = scanner.request(manifest);
    PROJECT_ASSET_CHECK(again != 0U && again != ticket);
    scanner.drain();
    const auto rescanned = scanner.take();
    PROJECT_ASSET_CHECK(rescanned.has_value());
    PROJECT_ASSET_CHECK(rescanned->ticket == again);
    PROJECT_ASSET_CHECK(rescanned->availability.missing == 0U);
    PROJECT_ASSET_CHECK(rescanned->availability.render_ready());
}

// Several questions put before the worker gets to them leave one answer, and
// it is the last question's: each earlier one was either answered and then
// replaced, or never started and counted.
void the_newest_question_is_the_one_answered() {
    TemporaryDirectory temporary;
    const HalfPresentShow show = half_present_show(temporary.path());
    const auto with_model = manifest_of(show.root);
    const auto erased = show.root.erase(doc::NodeId{21U});
    PROJECT_ASSET_CHECK(erased.has_value());
    const auto without_model = manifest_of(*erased);
    PROJECT_ASSET_CHECK(with_model.digest != without_model.digest);

    app::ProjectAssetScanner scanner;
    constexpr std::size_t questions = 12U;
    core::u64 last{};
    for (std::size_t index = 0U; index < questions; ++index) {
        last = scanner.request(index + 1U == questions ? without_model
                                                       : with_model);
        PROJECT_ASSET_CHECK(last != 0U);
    }
    scanner.drain();
    PROJECT_ASSET_CHECK(scanner.superseded() < questions);

    const auto answered = scanner.take();
    PROJECT_ASSET_CHECK(answered.has_value());
    PROJECT_ASSET_CHECK(answered->ticket == last);
    PROJECT_ASSET_CHECK(answered->availability.digest == without_model.digest);
    PROJECT_ASSET_CHECK(answered->availability.assets.size() == 1U);
    PROJECT_ASSET_CHECK(answered->availability.missing == 0U);
    PROJECT_ASSET_CHECK(!scanner.take().has_value());
}

// RESCAN's deeper look, on the same thread: the recorded revision is
// compared, and the Library files that changed come back with the answer.
void a_fingerprint_scan_compares_revisions_and_lists_changed_library_files() {
    TemporaryDirectory temporary;
    const fs::path changed_path = temporary.path() / "changed.mov";
    const fs::path steady_path = temporary.path() / "steady.mov";
    write(changed_path, "frames");
    write(steady_path, "frames");
    const std::string changed_source =
        assets::asset_source_from_path(changed_path);
    const std::string steady_source =
        assets::asset_source_from_path(steady_path);
    const auto recorded = assets::source_fingerprint(changed_source);
    const auto steady = assets::source_fingerprint(steady_source);
    PROJECT_ASSET_CHECK(recorded && steady);

    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, changed_source, recorded.value()));
    root = root.set(fingerprinted_asset_node(
        11U, assets::schema::media_type, steady_source, steady.value()));
    root = root.set(
        placement(20U, scene::schema::media_type, changed_source, 10U));
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry library = model.evaluate();
    const auto manifest = app::collect_project_asset_manifest(root, library);

    const auto original_time = fs::last_write_time(changed_path);
    write(changed_path, "new frames");
    fs::last_write_time(changed_path,
                        original_time + std::chrono::seconds{2});

    app::ProjectAssetScanner scanner;
    // The routine question sees a file that is there and nothing more.
    const core::u64 routine = scanner.request(manifest);
    scanner.drain();
    const auto present = scanner.take();
    PROJECT_ASSET_CHECK(present.has_value() && present->ticket == routine);
    PROJECT_ASSET_CHECK(present->availability.ready());
    PROJECT_ASSET_CHECK(present->changed_library_sources.empty());

    const core::u64 asked = scanner.request(
        manifest, app::ProjectAssetScanDepth::Fingerprints, library);
    scanner.drain();
    const auto checked = scanner.take();
    PROJECT_ASSET_CHECK(checked.has_value() && checked->ticket == asked);
    PROJECT_ASSET_CHECK(checked->depth ==
                        app::ProjectAssetScanDepth::Fingerprints);
    PROJECT_ASSET_CHECK(!checked->availability.ready());
    PROJECT_ASSET_CHECK(checked->availability.mismatched == 1U);
    PROJECT_ASSET_CHECK(checked->changed_library_sources.size() == 1U);
    PROJECT_ASSET_CHECK(checked->changed_library_sources[0U].asset ==
                        doc::NodeId{10U});
    PROJECT_ASSET_CHECK(
        checked->changed_library_sources[0U].observed.byte_length == 10U);
}

// Between asking and being answered, a machine goes on what it already
// knew: a missing file stays missing, and a file it has not been asked
// about before is taken to be there.
void an_unanswered_manifest_keeps_what_was_known_and_assumes_the_rest() {
    TemporaryDirectory temporary;
    const HalfPresentShow show = half_present_show(temporary.path());
    const auto first = manifest_of(show.root);
    const auto known = app::scan_project_assets(first);
    PROJECT_ASSET_CHECK(known.missing == 1U);

    const std::string new_source = assets::asset_source_from_path(
        temporary.path() / "never-checked.mov");
    const doc::Root grown = show.root.set(
        placement(22U, scene::schema::media_type, new_source));
    const auto second = manifest_of(grown);
    PROJECT_ASSET_CHECK(second.digest != first.digest);

    const auto provisional =
        app::provisional_asset_availability(second, known);
    PROJECT_ASSET_CHECK(provisional.digest == second.digest);
    PROJECT_ASSET_CHECK(provisional.assets.size() == 3U);
    PROJECT_ASSET_CHECK(provisional.missing == 1U);
    const auto* clip = presence_of(provisional, show.present_source);
    const auto* set = presence_of(provisional, show.absent_source);
    const auto* unknown = presence_of(provisional, new_source);
    PROJECT_ASSET_CHECK(clip != nullptr && clip->present);
    PROJECT_ASSET_CHECK(set != nullptr && !set->present);
    PROJECT_ASSET_CHECK(set->state ==
                        app::ProjectAssetPresence::State::Missing);
    // Not on disk at all, and assumed present until somebody looks.
    PROJECT_ASSET_CHECK(unknown != nullptr && unknown->present);
    PROJECT_ASSET_CHECK(!fs::exists(temporary.path() / "never-checked.mov"));

    // The scan itself is not fooled.
    PROJECT_ASSET_CHECK(app::scan_project_assets(second).missing == 2U);
    // With nothing known, everything is assumed.
    const auto cold = app::provisional_asset_availability(second, {});
    PROJECT_ASSET_CHECK(cold.missing == 0U && cold.assets.size() == 3U);
}

// What a frame loop sees. A machine does not call itself ready for a
// manifest it has not looked at, however complete its assumption is, and it
// finds a file that arrived later only when somebody asks it to look again.
void readiness_is_found_not_assumed_and_changes_only_when_asked() {
    TemporaryDirectory temporary;
    const HalfPresentShow show = half_present_show(temporary.path());
    const auto manifest = manifest_of(show.root);

    app::ProjectAssetReadiness readiness;
    PROJECT_ASSET_CHECK(!readiness.answered());
    PROJECT_ASSET_CHECK(!readiness.poll().has_value());
    // Nothing is held yet, so there is nothing to look at again.
    PROJECT_ASSET_CHECK(!readiness.recheck());

    PROJECT_ASSET_CHECK(readiness.require(manifest));
    PROJECT_ASSET_CHECK(readiness.digest() == manifest.digest);
    PROJECT_ASSET_CHECK(readiness.checking());
    // Assumed complete, and exactly for that reason not reported ready.
    PROJECT_ASSET_CHECK(readiness.availability().render_ready());
    PROJECT_ASSET_CHECK(!readiness.answered());
    PROJECT_ASSET_CHECK(!readiness.render_ready());
    // The same manifest again is not a new question.
    PROJECT_ASSET_CHECK(!readiness.require(manifest));

    readiness.drain();
    const auto first = readiness.poll();
    PROJECT_ASSET_CHECK(first.has_value());
    PROJECT_ASSET_CHECK(first->first_for_manifest);
    PROJECT_ASSET_CHECK(first->depth == app::ProjectAssetScanDepth::Presence);
    PROJECT_ASSET_CHECK(readiness.answered());
    PROJECT_ASSET_CHECK(!readiness.checking());
    PROJECT_ASSET_CHECK(readiness.availability().missing == 1U);
    PROJECT_ASSET_CHECK(!readiness.render_ready());
    PROJECT_ASSET_CHECK(!readiness.poll().has_value());

    // The model is copied over. Nobody has asked, so nothing has looked.
    write(temporary.path() / "set.glb", "mesh");
    readiness.drain();
    PROJECT_ASSET_CHECK(!readiness.poll().has_value());
    PROJECT_ASSET_CHECK(readiness.availability().missing == 1U);
    PROJECT_ASSET_CHECK(!readiness.render_ready());

    // RESCAN. What was known stands until the answer arrives.
    PROJECT_ASSET_CHECK(readiness.recheck());
    PROJECT_ASSET_CHECK(readiness.answered());
    PROJECT_ASSET_CHECK(readiness.availability().missing == 1U);
    readiness.drain();
    const auto again = readiness.poll();
    PROJECT_ASSET_CHECK(again.has_value());
    PROJECT_ASSET_CHECK(!again->first_for_manifest);
    PROJECT_ASSET_CHECK(readiness.availability().missing == 0U);
    PROJECT_ASSET_CHECK(readiness.render_ready());

    // And the other way: a file that went away is reported when asked.
    fs::remove(temporary.path() / "set.glb");
    PROJECT_ASSET_CHECK(readiness.render_ready());
    PROJECT_ASSET_CHECK(readiness.recheck());
    readiness.drain();
    PROJECT_ASSET_CHECK(readiness.poll().has_value());
    PROJECT_ASSET_CHECK(!readiness.render_ready());
}

// A manifest that changes while its predecessor is still being looked at:
// the earlier answer is for a question nobody is asking any more.
void an_answer_to_a_replaced_question_is_never_adopted() {
    TemporaryDirectory temporary;
    const HalfPresentShow show = half_present_show(temporary.path());
    const auto with_model = manifest_of(show.root);
    const auto erased = show.root.erase(doc::NodeId{21U});
    PROJECT_ASSET_CHECK(erased.has_value());
    const auto without_model = manifest_of(*erased);

    for (int round = 0; round < 20; ++round) {
        app::ProjectAssetReadiness readiness;
        PROJECT_ASSET_CHECK(readiness.require(with_model));
        PROJECT_ASSET_CHECK(readiness.require(without_model));
        readiness.drain();
        const auto answer = readiness.poll();
        PROJECT_ASSET_CHECK(answer.has_value());
        PROJECT_ASSET_CHECK(answer->first_for_manifest);
        PROJECT_ASSET_CHECK(readiness.digest() == without_model.digest);
        PROJECT_ASSET_CHECK(readiness.availability().digest ==
                            without_model.digest);
        PROJECT_ASSET_CHECK(readiness.availability().assets.size() == 1U);
        // Without the model the show is complete; with it, it was not.
        PROJECT_ASSET_CHECK(readiness.render_ready());
        PROJECT_ASSET_CHECK(!readiness.poll().has_value());
    }
}

// The operator's deeper check survives a manifest change made while it is
// out: the answer that arrives is still the one RESCAN asked for.
void a_rescan_keeps_its_depth_when_the_manifest_changes_under_it() {
    TemporaryDirectory temporary;
    const fs::path clip_path = temporary.path() / "plate.mov";
    write(clip_path, "frames");
    const std::string source = assets::asset_source_from_path(clip_path);
    const auto recorded = assets::source_fingerprint(source);
    PROJECT_ASSET_CHECK(recorded);
    doc::Root root;
    root = root.set(fingerprinted_asset_node(
        10U, assets::schema::media_type, source, recorded.value()));
    root = root.set(placement(20U, scene::schema::media_type, source, 10U));
    const auto library_of = [](const doc::Root& document) {
        assets::AssetModel model;
        model.reset(document);
        return model.evaluate();
    };
    const auto before = app::collect_project_asset_manifest(
        root, library_of(root));
    const std::string second_source = assets::asset_source_from_path(
        temporary.path() / "absent.glb");
    const doc::Root grown =
        root.set(placement(21U, scene::schema::model_type, second_source));
    const auto after = app::collect_project_asset_manifest(
        grown, library_of(grown));
    PROJECT_ASSET_CHECK(before.digest != after.digest);

    // The plate is replaced on disk after it was recorded.
    const auto original_time = fs::last_write_time(clip_path);
    write(clip_path, "longer frames");
    fs::last_write_time(clip_path, original_time + std::chrono::seconds{2});

    app::ProjectAssetReadiness readiness;
    PROJECT_ASSET_CHECK(readiness.require(before, library_of(root)));
    readiness.drain();
    const auto routine = readiness.poll();
    PROJECT_ASSET_CHECK(routine.has_value());
    // Presence alone cannot see a replaced file.
    PROJECT_ASSET_CHECK(readiness.availability().ready());

    PROJECT_ASSET_CHECK(readiness.recheck(
        app::ProjectAssetScanDepth::Fingerprints, library_of(root)));
    PROJECT_ASSET_CHECK(readiness.require(after, library_of(grown)));
    readiness.drain();
    const auto deep = readiness.poll();
    PROJECT_ASSET_CHECK(deep.has_value());
    PROJECT_ASSET_CHECK(deep->depth ==
                        app::ProjectAssetScanDepth::Fingerprints);
    PROJECT_ASSET_CHECK(deep->first_for_manifest);
    PROJECT_ASSET_CHECK(readiness.availability().digest == after.digest);
    PROJECT_ASSET_CHECK(readiness.availability().mismatched == 1U);
    PROJECT_ASSET_CHECK(readiness.availability().missing == 1U);
    PROJECT_ASSET_CHECK(deep->changed_library_sources.size() == 1U);
    PROJECT_ASSET_CHECK(deep->changed_library_sources[0U].asset ==
                        doc::NodeId{10U});

    // Once answered, the routine depth is the routine again.
    const auto erased = grown.erase(doc::NodeId{21U});
    PROJECT_ASSET_CHECK(erased.has_value());
    PROJECT_ASSET_CHECK(readiness.require(before, library_of(*erased)));
    readiness.drain();
    const auto back = readiness.poll();
    PROJECT_ASSET_CHECK(back.has_value());
    PROJECT_ASSET_CHECK(back->depth == app::ProjectAssetScanDepth::Presence);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"the scanner reports what is on disk for the manifest asked",
     &the_scanner_reports_what_is_on_disk_for_the_manifest_asked},
    {"the newest question is the one answered",
     &the_newest_question_is_the_one_answered},
    {"a fingerprint scan compares revisions and lists changed Library files",
     &a_fingerprint_scan_compares_revisions_and_lists_changed_library_files},
    {"an unanswered manifest keeps what was known and assumes the rest",
     &an_unanswered_manifest_keeps_what_was_known_and_assumes_the_rest},
    {"readiness is found, not assumed, and changes only when asked",
     &readiness_is_found_not_assumed_and_changes_only_when_asked},
    {"an answer to a replaced question is never adopted",
     &an_answer_to_a_replaced_question_is_never_adopted},
    {"a RESCAN keeps its depth when the manifest changes under it",
     &a_rescan_keeps_its_depth_when_the_manifest_changes_under_it},
    {"RESCAN finds changed Library sources until they are accepted",
     &rescan_finds_changed_library_sources_until_they_are_accepted},
    {"an image sequence is present when its first frame is",
     &an_image_sequence_is_present_when_its_first_frame_is},
    {"colour LUTs participate in the project manifest",
     &colour_luts_participate_in_the_project_manifest},
    {"IES sources participate in the project manifest",
     &ies_sources_participate_in_the_project_manifest},
    {"particle packages participate in the project manifest",
     &particle_packages_participate_in_the_project_manifest},
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
    {"every plate of a plateset take has to be opened",
     &every_plate_of_a_plateset_take_has_to_be_opened},
    {"plate properties count only on a clip from a plateset",
     &plate_properties_count_only_on_a_clip_from_a_plateset},
    {"a plate revision is judged by size and time alone",
     &a_plate_revision_is_judged_by_size_and_time_alone},
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
