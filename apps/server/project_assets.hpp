#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
    assets::AssetKind kind{assets::AssetKind::Media};

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

    // Missing media has a visible renderer fallback. Other asset failures
    // still prevent runtime adoption; ready() remains the store-health answer.
    [[nodiscard]] bool render_ready() const noexcept {
        if (mismatched != 0U || verification_failed != 0U) return false;
        core::u32 offline_media{};
        for (const auto& presence : assets) {
            if (presence.state == ProjectAssetPresence::State::Missing &&
                presence.kind == assets::AssetKind::Media) ++offline_media;
        }
        return missing == offline_media;
    }

    bool operator==(const ProjectAssetAvailability&) const = default;
};

// Compares every required source on this machine with the revision recorded
// for it. An unfingerprinted asset needs only a stat; a fingerprinted one has
// its size and modification time compared with what was recorded, and a
// difference is reported apart from a missing or unreadable file. No file's
// contents are read.
[[nodiscard]] ProjectAssetAvailability check_project_assets(
    const ProjectAssetManifest& manifest, std::stop_token stop = {});

// Can this machine open every required source? One stat per asset and no
// reads, which is what the routine path wants: a show is not deployed and left
// alone, it is rebuilt constantly, and a file's bytes are not in doubt. The
// verdict is Ready or Missing and never a content mismatch, because nothing
// here looks at content.
//
// check_project_assets is the deliberate, operator-asked version of the same
// question. Neither is called on a frame path: both ask the file system, and
// a file server that is slow to answer would hold the show for as long as it
// took. ProjectAssetScanner below asks from a thread of its own.
//
// `stop` ends a scan early; what it returns then is incomplete and is for
// nobody.
[[nodiscard]] ProjectAssetAvailability scan_project_assets(
    const ProjectAssetManifest& manifest, std::stop_token stop = {});

// What a machine goes on until the scanner answers for a manifest it has not
// looked at yet. A requirement the previous answer already covers keeps that
// answer; a new one is taken to be present, so a clip that was just placed is
// opened at once and falls back to the decoder's own placeholder if the file
// turns out not to be there, instead of being drawn as missing for the frames
// the question takes. It is an assumption and is never reported as readiness.
[[nodiscard]] ProjectAssetAvailability provisional_asset_availability(
    const ProjectAssetManifest& manifest,
    const ProjectAssetAvailability& previous);

// A Library asset whose file is no longer the revision it recorded: what was
// found at its source when the operator last asked.
struct ChangedLibrarySource final {
    doc::NodeId asset{};
    assets::ContentFingerprint observed;

    bool operator==(const ChangedLibrarySource&) const = default;
};

// Every fingerprinted Library asset - used or not - whose source now has a
// different size or modification time than it recorded. One stat per asset
// and no reads; a missing source is reported elsewhere, not here. Called
// only when the operator asks (RESCAN), never on the frame path.
[[nodiscard]] std::vector<ChangedLibrarySource> find_changed_library_sources(
    const assets::AssetRegistry& registry, std::stop_token stop = {});

// The assets in `found` that still differ from what the registry records.
// No file access: an accepted revision drops out as soon as the document
// records the fingerprint that was found.
[[nodiscard]] std::vector<doc::NodeId> still_changed_library_sources(
    const assets::AssetRegistry& registry,
    const std::vector<ChangedLibrarySource>& found);

// How hard a scan looks. Presence is the routine question; Fingerprints is
// the operator's RESCAN, which also compares each file's recorded size and
// modification time and lists the Library files that changed.
enum class ProjectAssetScanDepth : core::u8 {
    Presence,
    Fingerprints,
};

struct ProjectAssetScanResult final {
    // The request this answers, as request() returned it.
    core::u64 ticket{};
    ProjectAssetScanDepth depth{ProjectAssetScanDepth::Presence};
    ProjectAssetAvailability availability;
    // Fingerprints only.
    std::vector<ChangedLibrarySource> changed_library_sources;
};

// Asks the file system about a manifest on a thread of its own, so the frame
// loop that wants the answer never waits for a disk or a file server.
//
// A scan runs only when it is asked for - when the set of required files
// changes, and when the operator presses RESCAN. Nothing here repeats one on
// a timer. The newest request is the one that matters: a request not yet
// started is replaced by a newer one, and a caller compares the ticket of
// what it takes with the last one it was given, so an answer to a question it
// has since replaced is never adopted.
class ProjectAssetScanner final {
  public:
    ProjectAssetScanner();
    ProjectAssetScanner(const ProjectAssetScanner&) = delete;
    ProjectAssetScanner& operator=(const ProjectAssetScanner&) = delete;
    // Stops after the file being looked at; a scan in progress is abandoned.
    ~ProjectAssetScanner();

    // Queues a scan and returns its ticket, or 0 when it could not be
    // queued. `library` is read only by a Fingerprints scan. Never touches
    // the disk and never waits for the worker.
    [[nodiscard]] core::u64 request(
        const ProjectAssetManifest& manifest,
        ProjectAssetScanDepth depth = ProjectAssetScanDepth::Presence,
        const assets::AssetRegistry& library = {}) noexcept;

    // The newest finished scan not yet taken. Never blocks: while the worker
    // is publishing one it answers nothing, and the next frame asks again.
    [[nodiscard]] std::optional<ProjectAssetScanResult> take() noexcept;

    // Requests replaced before the worker started them.
    [[nodiscard]] core::u64 superseded() const noexcept {
        return superseded_.load(std::memory_order_relaxed);
    }

    // Blocks until nothing is queued or running. Tests only.
    void drain();

  private:
    struct Job final {
        core::u64 ticket{};
        ProjectAssetScanDepth depth{ProjectAssetScanDepth::Presence};
        ProjectAssetManifest manifest;
        assets::AssetRegistry library;
    };

    void run(std::stop_token stop);

    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::condition_variable idle_;
    std::optional<Job> queued_;
    std::optional<ProjectAssetScanResult> finished_;
    core::u64 next_ticket_{1U};
    bool busy_{};
    std::atomic<core::u64> superseded_{};
    std::jthread worker_;
};

// A frame loop's side of the asset check: what this machine may assume about
// the files the committed document needs, and whether it has actually looked.
//
// The Director's loop and a node's use it the same way. require() is given
// the manifest whenever the document changes and asks the scanner when the
// manifest is a new one. recheck() is the operator's RESCAN. poll() adopts an
// answer on the frame that finds it. None of the three waits for a disk, and
// nothing here asks on its own: a file that appears later is found by the
// next RESCAN and not before.
class ProjectAssetReadiness final {
  public:
    struct Answer final {
        ProjectAssetScanDepth depth{ProjectAssetScanDepth::Presence};
        // The first answer for this manifest, as opposed to a re-check of
        // one already answered.
        bool first_for_manifest{};
        // Fingerprints only.
        std::vector<ChangedLibrarySource> changed_library_sources;
    };

    // The manifest the committed document needs now. Returns whether it is a
    // different one from the manifest held, in which case the question is
    // put and readiness is withdrawn until it is answered.
    bool require(const ProjectAssetManifest& manifest,
                 const assets::AssetRegistry& library = {});

    // Looks again at the manifest held, because the operator asked. What is
    // known stands until the answer arrives. A Fingerprints re-check stays
    // wanted until one is answered, so a manifest that changes meanwhile is
    // checked at that depth too. False when the question could not be put.
    bool recheck(ProjectAssetScanDepth depth = ProjectAssetScanDepth::Presence,
                 const assets::AssetRegistry& library = {});

    // Adopts the answer to the latest question, if it has arrived. An answer
    // to a question since replaced is dropped. Never blocks.
    [[nodiscard]] std::optional<Answer> poll();

    // What to go on: the last answer, or for a manifest not answered yet,
    // what was known before with anything new assumed present.
    [[nodiscard]] const ProjectAssetAvailability& availability() const noexcept {
        return availability_;
    }
    [[nodiscard]] core::u64 digest() const noexcept { return manifest_.digest; }
    // Whether this machine has looked at the manifest it holds.
    [[nodiscard]] bool answered() const noexcept {
        return held_ && answered_digest_ == manifest_.digest && answered_any_;
    }
    // Ready to render, as found and not as assumed.
    [[nodiscard]] bool render_ready() const noexcept {
        return answered() && availability_.render_ready();
    }
    // A question is out and its answer has not been adopted.
    [[nodiscard]] bool checking() const noexcept { return ticket_ != 0U; }

    // Blocks until the scanner is idle. Tests only.
    void drain() { scanner_.drain(); }

  private:
    bool ask(const assets::AssetRegistry& library);

    ProjectAssetScanner scanner_;
    ProjectAssetManifest manifest_;
    ProjectAssetAvailability availability_;
    bool held_{};
    // The question whose answer is awaited; 0 when none is.
    core::u64 ticket_{};
    core::u64 answered_digest_{};
    bool answered_any_{};
    ProjectAssetScanDepth wanted_{ProjectAssetScanDepth::Presence};
};

} // namespace mediaserver::app
