#include "project_assets.hpp"

#include <mediaserver/assets/asset_path.hpp>

#include <mediaserver/core/fnv1a.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <limits>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

namespace mediaserver::app {
namespace {

void mix_fingerprint(
    core::u64& state,
    const std::optional<assets::ContentFingerprint>& fingerprint) noexcept {
    core::fnv1a_byte(state, fingerprint.has_value() ? 1U : 0U);
    if (!fingerprint.has_value()) {
        return;
    }
    core::fnv1a_u64(state, fingerprint->byte_length);
    for (const core::u8 byte : fingerprint->sha256.bytes) {
        core::fnv1a_byte(state, byte);
    }
}

void increment(core::u32& value) noexcept {
    if (value != std::numeric_limits<core::u32>::max()) {
        ++value;
    }
}

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string text(const doc::PropertyMap& properties,
                               std::string_view name) {
    const auto* const value = find(properties, name);
    const auto* const held = value == nullptr
        ? nullptr
        : std::get_if<std::string>(value);
    return held == nullptr ? std::string{} : *held;
}

[[nodiscard]] std::optional<doc::NodeId> node_id(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto* const value = find(properties, name);
    const auto* const held = value == nullptr
        ? nullptr
        : std::get_if<std::int64_t>(value);
    if (held == nullptr || *held < 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*held)};
}

} // namespace

ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry) {
    ProjectAssetManifest manifest{};
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    manifest.assets.reserve(contents.changes.size());

    for (const doc::NodeChange& change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node& node = *change.after;
        const auto include_color = [&](std::string_view property) {
            auto path = text(node.properties, property);
            if (assets::valid_asset_source(path)) manifest.assets.push_back(RequiredProjectAsset{
                {}, assets::AssetKind::ColorLut, std::move(path), {}});
        };
        if (node.type == output::schema::surface_type) include_color(output::schema::camera_inverse_lut);
        if (node.type == scene::schema::settings_type) include_color(scene::schema::show_lut);
        const auto add_required = [&](assets::AssetKind kind, std::string source,
                                      std::optional<doc::NodeId> authored_asset) {
            if (!assets::valid_asset_source(source)) {
                return;
            }
            // An image sequence is a '#' run resolved per frame. A machine has
            // it when its first frame opens - one stat, like any other source -
            // and there is no single file whose revision a fingerprint could
            // name.
            if (const auto run = source.find('#');
                kind == assets::AssetKind::Media && run != std::string::npos) {
                auto end = run;
                while (end < source.size() && source[end] == '#') ++end;
                const auto* start = find(node.properties, scene::schema::sequence_start);
                const auto* first = start == nullptr ? nullptr : std::get_if<std::int64_t>(start);
                auto number = std::to_string(first != nullptr && *first > 0 ? *first : 0);
                if (number.size() < end - run) number.insert(0U, end - run - number.size(), '0');
                manifest.assets.push_back(RequiredProjectAsset{
                    {}, kind, source.substr(0U, run) + number + source.substr(end), {}});
                return;
            }

            // A placement's asset_id is the strongest link; exact-source lookup
            // repairs a placement whose record was deleted and re-imported. A
            // placement with no record at all is still a file this show opens,
            // so it belongs in the manifest either way - which is what keeps a
            // node and the Director agreeing about what has to be present.
            const assets::Asset* asset = authored_asset.has_value()
                ? registry.asset(*authored_asset)
                : nullptr;
            if (asset != nullptr && assets::asset_kind(*asset) != kind) {
                if (manifest.kind_mismatches !=
                    std::numeric_limits<core::u32>::max()) {
                    ++manifest.kind_mismatches;
                }
                asset = nullptr;
            }
            if (asset == nullptr) {
                asset = registry.asset_for_source(source, kind);
            }
            manifest.assets.push_back(RequiredProjectAsset{
                asset == nullptr ? doc::NodeId{} : assets::asset_id(*asset), kind,
                std::move(source),
                asset == nullptr
                    ? std::optional<assets::ContentFingerprint>{}
                    : assets::asset_fingerprint(*asset)});
        };
        assets::AssetKind kind{};
        std::string source;
        std::optional<doc::NodeId> authored_asset;
        bool relevant = false;
        if (node.type == scene::schema::light_type) {
            relevant = true;
            kind = assets::AssetKind::IesProfile;
            source = text(node.properties, scene::schema::ies_source);
        } else if (node.type == scene::schema::media_type) {
            relevant = true;
            kind = assets::AssetKind::Media;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
            // A plateset take is one clip that plays a plate on every slot
            // screen of its vehicle track. `source` is only its front plate;
            // each slot's file is its own required asset, or a node never
            // learns it has the file and leaves that wall unlit. Read only
            // from a clip that says it came from a plateset, as the scene
            // model does. The front plate repeats `source` and is folded into
            // one entry by the de-duplication below.
            if (find(node.properties, scene::schema::plateset_asset) != nullptr) {
                for (std::size_t slot = 0U;
                     slot < scene::schema::vehicle_slot_count; ++slot) {
                    add_required(
                        assets::AssetKind::Media,
                        text(node.properties,
                             scene::schema::plate_property(
                                 slot, scene::schema::plate_source_suffix)),
                        node_id(node.properties,
                                scene::schema::plate_property(
                                    slot, scene::schema::plate_asset_suffix)));
                }
            }
        } else if (node.type == scene::schema::model_type) {
            relevant = true;
            kind = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::schema::particle_effect_type) {
            relevant = true;
            kind = assets::AssetKind::ParticleEffect;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::link_schema::prototype_type) {
            // Geometry Blender sent: the ingested package every linked
            // occurrence of this prototype draws.
            relevant = true;
            kind = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
        } else if (node.type == output::schema::surface_type &&
                   text(node.properties, output::schema::surface_shape) ==
                       output::schema::shape_imported_mesh) {
            relevant = true;
            kind = assets::AssetKind::ProjectionMesh;
            source = text(node.properties, output::schema::mesh_source);
            authored_asset =
                node_id(node.properties, output::schema::mesh_asset_id);
        }
        if (relevant) {
            add_required(kind, std::move(source), authored_asset);
        }
    }

    const auto order = [](const RequiredProjectAsset& left,
                          const RequiredProjectAsset& right) noexcept {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        return left.source < right.source;
    };
    std::sort(manifest.assets.begin(), manifest.assets.end(), order);
    manifest.assets.erase(
        std::unique(manifest.assets.begin(), manifest.assets.end(),
                    [](const RequiredProjectAsset& left,
                       const RequiredProjectAsset& right) noexcept {
                        return left.kind == right.kind &&
                            left.source == right.source;
                    }),
        manifest.assets.end());

    core::u64 state = core::fnv1a_offset_short;
    core::fnv1a_u64(state, manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        core::fnv1a_byte(state, static_cast<core::u8>(required.kind));
        core::fnv1a_text(state, required.source);
        mix_fingerprint(state, required.expected_fingerprint);
    }
    core::fnv1a_u64(state, manifest.kind_mismatches);
    manifest.digest = state;
    return manifest;
}

ProjectAssetAvailability check_project_assets(
    const ProjectAssetManifest& manifest, std::stop_token stop) {
    ProjectAssetAvailability availability{};
    availability.digest = manifest.digest;
    availability.assets.reserve(manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        if (stop.stop_requested()) {
            break;
        }
        ProjectAssetPresence presence{};
        presence.asset = required.asset;
        presence.kind = required.kind;
        presence.source = required.source;
        presence.expected_fingerprint = required.expected_fingerprint;

        if (!required.expected_fingerprint.has_value()) {
            presence.present = assets::asset_source_present(required.source);
            presence.state = presence.present
                ? ProjectAssetPresence::State::Ready
                : ProjectAssetPresence::State::Missing;
            if (!presence.present) {
                increment(availability.missing);
            }
            availability.assets.push_back(std::move(presence));
            continue;
        }

        auto observed = assets::source_fingerprint(required.source);
        if (!observed) {
            if (observed.error().code ==
                    assets::ContentFingerprintErrorCode::SourceMissing ||
                observed.error().code ==
                    assets::ContentFingerprintErrorCode::InvalidSource) {
                presence.state = ProjectAssetPresence::State::Missing;
                increment(availability.missing);
            } else {
                presence.present = true;
                presence.state =
                    ProjectAssetPresence::State::VerificationFailed;
                increment(availability.verification_failed);
            }
            availability.assets.push_back(std::move(presence));
            continue;
        }

        presence.present = true;
        presence.observed_fingerprint = observed.value();
        if (observed.value().byte_length !=
            required.expected_fingerprint->byte_length) {
            presence.state =
                ProjectAssetPresence::State::ByteLengthMismatch;
            increment(availability.mismatched);
        } else if (observed.value().sha256 !=
                   required.expected_fingerprint->sha256) {
            presence.state = ProjectAssetPresence::State::Sha256Mismatch;
            increment(availability.mismatched);
        } else {
            presence.state = ProjectAssetPresence::State::Ready;
        }
        availability.assets.push_back(std::move(presence));
    }
    return availability;
}

std::vector<ChangedLibrarySource> find_changed_library_sources(
    const assets::AssetRegistry& registry, std::stop_token stop) {
    std::vector<ChangedLibrarySource> changed;
    for (const assets::Asset& asset : registry.assets) {
        if (stop.stop_requested()) {
            break;
        }
        const std::optional<assets::ContentFingerprint>& recorded =
            assets::asset_fingerprint(asset);
        if (!recorded.has_value()) {
            continue;
        }
        auto observed =
            assets::source_fingerprint(assets::asset_source(asset));
        if (!observed || observed.value() == *recorded) {
            continue;
        }
        changed.push_back(ChangedLibrarySource{assets::asset_id(asset),
                                               std::move(observed).value()});
    }
    return changed;
}

std::vector<doc::NodeId> still_changed_library_sources(
    const assets::AssetRegistry& registry,
    const std::vector<ChangedLibrarySource>& found) {
    std::vector<doc::NodeId> changed;
    for (const ChangedLibrarySource& entry : found) {
        const assets::Asset* const asset = registry.asset(entry.asset);
        if (asset == nullptr) {
            continue;
        }
        const std::optional<assets::ContentFingerprint>& recorded =
            assets::asset_fingerprint(*asset);
        if (recorded.has_value() && *recorded != entry.observed) {
            changed.push_back(entry.asset);
        }
    }
    return changed;
}

ProjectAssetAvailability scan_project_assets(
    const ProjectAssetManifest& manifest, std::stop_token stop) {
    ProjectAssetAvailability availability{};
    availability.digest = manifest.digest;
    availability.assets.reserve(manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        if (stop.stop_requested()) {
            break;
        }
        ProjectAssetPresence presence{};
        presence.asset = required.asset;
        presence.kind = required.kind;
        presence.source = required.source;
        presence.expected_fingerprint = required.expected_fingerprint;
        presence.present = assets::asset_source_present(required.source);
        presence.state = presence.present
            ? ProjectAssetPresence::State::Ready
            : ProjectAssetPresence::State::Missing;
        if (!presence.present) {
            increment(availability.missing);
        }
        availability.assets.push_back(std::move(presence));
    }
    return availability;
}

ProjectAssetAvailability provisional_asset_availability(
    const ProjectAssetManifest& manifest,
    const ProjectAssetAvailability& previous) {
    ProjectAssetAvailability availability{};
    availability.digest = manifest.digest;
    availability.assets.reserve(manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        const auto known = std::find_if(
            previous.assets.begin(), previous.assets.end(),
            [&](const ProjectAssetPresence& presence) {
                return presence.asset == required.asset &&
                       presence.kind == required.kind &&
                       presence.source == required.source &&
                       presence.expected_fingerprint ==
                           required.expected_fingerprint;
            });
        ProjectAssetPresence presence{};
        if (known != previous.assets.end()) {
            presence = *known;
        } else {
            presence.asset = required.asset;
            presence.kind = required.kind;
            presence.source = required.source;
            presence.expected_fingerprint = required.expected_fingerprint;
            presence.present = true;
            presence.state = ProjectAssetPresence::State::Ready;
        }
        switch (presence.state) {
        case ProjectAssetPresence::State::Ready:
            break;
        case ProjectAssetPresence::State::Missing:
            increment(availability.missing);
            break;
        case ProjectAssetPresence::State::ByteLengthMismatch:
        case ProjectAssetPresence::State::Sha256Mismatch:
            increment(availability.mismatched);
            break;
        case ProjectAssetPresence::State::VerificationFailed:
            increment(availability.verification_failed);
            break;
        }
        availability.assets.push_back(std::move(presence));
    }
    return availability;
}

ProjectAssetScanner::ProjectAssetScanner()
    : worker_([this](std::stop_token stop) { run(stop); }) {}

ProjectAssetScanner::~ProjectAssetScanner() {
    worker_.request_stop();
    wake_.notify_all();
}

core::u64 ProjectAssetScanner::request(
    const ProjectAssetManifest& manifest, ProjectAssetScanDepth depth,
    const assets::AssetRegistry& library) noexcept {
    try {
        // Copied before the lock, which the worker takes only to pick a job
        // up or to hand an answer back - never while it is on the disk.
        Job job{0U, depth, manifest,
                depth == ProjectAssetScanDepth::Fingerprints
                    ? library
                    : assets::AssetRegistry{}};
        core::u64 ticket{};
        {
            const std::lock_guard lock{mutex_};
            ticket = next_ticket_++;
            job.ticket = ticket;
            if (queued_.has_value()) {
                superseded_.fetch_add(1U, std::memory_order_relaxed);
            }
            queued_ = std::move(job);
        }
        wake_.notify_one();
        return ticket;
    } catch (...) {
        return 0U;
    }
}

std::optional<ProjectAssetScanResult> ProjectAssetScanner::take() noexcept {
    const std::unique_lock lock{mutex_, std::try_to_lock};
    if (!lock.owns_lock() || !finished_.has_value()) {
        return std::nullopt;
    }
    std::optional<ProjectAssetScanResult> taken = std::move(finished_);
    finished_.reset();
    return taken;
}

void ProjectAssetScanner::drain() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return !queued_.has_value() && !busy_; });
}

void ProjectAssetScanner::run(std::stop_token stop) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            busy_ = false;
            if (!queued_.has_value()) {
                idle_.notify_all();
            }
            wake_.wait(lock, stop, [this] { return queued_.has_value(); });
            if (stop.stop_requested()) {
                return;
            }
            job = std::move(*queued_);
            queued_.reset();
            busy_ = true;
        }
        ProjectAssetScanResult result{};
        result.ticket = job.ticket;
        result.depth = job.depth;
        try {
            if (job.depth == ProjectAssetScanDepth::Fingerprints) {
                result.availability = check_project_assets(job.manifest, stop);
                result.changed_library_sources =
                    find_changed_library_sources(job.library, stop);
            } else {
                result.availability = scan_project_assets(job.manifest, stop);
            }
        } catch (...) {
            // No answer is better than a partial one: the caller keeps what
            // it had, and the next request asks again.
            continue;
        }
        if (stop.stop_requested()) {
            return;
        }
        const std::lock_guard lock{mutex_};
        finished_ = std::move(result);
    }
}

bool ProjectAssetReadiness::ask(const assets::AssetRegistry& library) {
    ticket_ = scanner_.request(manifest_, wanted_, library);
    return ticket_ != 0U;
}

bool ProjectAssetReadiness::require(const ProjectAssetManifest& manifest,
                                    const assets::AssetRegistry& library) {
    if (held_ && manifest.digest == manifest_.digest) {
        return false;
    }
    availability_ = provisional_asset_availability(manifest, availability_);
    manifest_ = manifest;
    held_ = true;
    static_cast<void>(ask(library));
    return true;
}

bool ProjectAssetReadiness::recheck(ProjectAssetScanDepth depth,
                                    const assets::AssetRegistry& library) {
    if (!held_) {
        return false;
    }
    if (depth == ProjectAssetScanDepth::Fingerprints) {
        wanted_ = depth;
    }
    return ask(library);
}

std::optional<ProjectAssetReadiness::Answer> ProjectAssetReadiness::poll() {
    if (ticket_ == 0U) {
        return std::nullopt;
    }
    auto scanned = scanner_.take();
    if (!scanned.has_value() || scanned->ticket != ticket_ ||
        scanned->availability.digest != manifest_.digest) {
        return std::nullopt;
    }
    ticket_ = 0U;
    Answer answer{};
    answer.depth = scanned->depth;
    answer.first_for_manifest = !answered();
    answer.changed_library_sources =
        std::move(scanned->changed_library_sources);
    availability_ = std::move(scanned->availability);
    answered_digest_ = manifest_.digest;
    answered_any_ = true;
    if (answer.depth == ProjectAssetScanDepth::Fingerprints) {
        wanted_ = ProjectAssetScanDepth::Presence;
    }
    return answer;
}

} // namespace mediaserver::app
