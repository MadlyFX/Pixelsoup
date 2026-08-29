#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mediaserver::app {

// One file the current document needs open on every render machine, derived
// from scene.media, scene.model, and imported output.surface references.
//
// Entries are deduplicated by (kind, source) and sorted the same way, so the
// Director and every node compute an identical manifest from the same
// document. Library assets nothing currently uses are not included, so an
// operator can keep unused imports without a node reporting them missing.
struct RequiredProjectAsset final {
    // The Library record this reference resolved to, or zero when a placement
    // carries a source with no Library row. Both are opened identically; the
    // record only supplies the display name and probe metadata.
    doc::NodeId asset{};
    assets::AssetKind kind{assets::AssetKind::Media};
    // Absolute path, the same on every machine in the show.
    std::string source;
    // The revision persistent Library metadata expects at `source`. Assets
    // authored before ingest fingerprints remain usable but can prove only
    // presence until Relink or Accept New Revision supplies this value.
    std::optional<assets::ContentFingerprint> expected_fingerprint;

    bool operator==(const RequiredProjectAsset&) const = default;
};

struct ProjectAssetManifest final {
    std::vector<RequiredProjectAsset> assets;
    // Placements whose asset_id names a Library record of a different kind -
    // an authoring error, not a distribution one.
    core::u32 kind_mismatches{};
    // Deterministic key over every required (kind, source, expected revision).
    // Playback uses it to skip rechecking after unrelated document edits, and
    // nodes report it back so the Director knows which manifest a readiness
    // answer refers to.
    core::u64 digest{};

    [[nodiscard]] bool empty() const noexcept {
        return assets.empty() && kind_mismatches == 0U;
    }

    bool operator==(const ProjectAssetManifest&) const = default;
};

[[nodiscard]] ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry);

// Enough of a file's identity to answer "is this still the thing I looked
// at?" without reading it. Cheap where a fingerprint is not.
struct ProjectAssetStamp final {
    core::u64 byte_size{};
    core::i64 modified_ticks{};

    bool operator==(const ProjectAssetStamp&) const = default;
};

struct ProjectAssetPresence final {
    doc::NodeId asset{};
    std::string source;
    bool present{};
    enum class State : core::u8 {
        Ready,
        Missing,
        ByteLengthMismatch,
        Sha256Mismatch,
        VerificationFailed,
    } state{State::Missing};
    std::optional<assets::ContentFingerprint> expected_fingerprint;
    std::optional<assets::ContentFingerprint> observed_fingerprint;
    // What the file looked like when this verdict was reached: byte size and
    // write time, or nothing when there was no file. A retry that finds the
    // same stamp reuses the verdict rather than reading the source again,
    // which is what keeps a periodic recheck off a large file's bytes.
    std::optional<ProjectAssetStamp> stamp;

    bool operator==(const ProjectAssetPresence&) const = default;
};

// What one machine found when it looked. `digest` binds the answer to the
// exact manifest it was asked about, so a stale report can never be read as
// readiness for a newer one.
struct ProjectAssetAvailability final {
    core::u64 digest{};
    std::vector<ProjectAssetPresence> assets;
    core::u32 missing{};
    core::u32 mismatched{};
    core::u32 verification_failed{};

    [[nodiscard]] bool ready() const noexcept {
        return missing == 0U && mismatched == 0U &&
            verification_failed == 0U;
    }

    bool operator==(const ProjectAssetAvailability&) const = default;
};

// Verifies every required source on this machine. Unfingerprinted assets need
// only a stat; fingerprinted assets stream through SHA-256 and report size and
// content mismatches separately from missing or unreadable files.
[[nodiscard]] ProjectAssetAvailability check_project_assets(
    const ProjectAssetManifest& manifest);

// The same question asked again a few seconds later, given the last answer.
// A source whose size and write time are unchanged keeps its previous verdict,
// whatever that verdict was: its bytes cannot have produced a different answer
// since, and streaming a large source through SHA-256 costs seconds on the
// thread that asks. Only sources that appeared, vanished, or changed are read
// again - which is exactly what a retry is for. This makes a recheck cost one
// stat per asset rather than a full read of every fingerprinted one, so a
// project that is missing or mismatched somewhere does not stall a frame every
// retry for as long as its largest source takes to hash.
[[nodiscard]] ProjectAssetAvailability recheck_project_assets(
    const ProjectAssetManifest& manifest,
    const ProjectAssetAvailability& previous);

} // namespace mediaserver::app
