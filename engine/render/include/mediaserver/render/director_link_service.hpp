#pragma once
#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/render/linked_stage_context.hpp>
#include <memory>
#include <chrono>
#include <string>

namespace mediaserver::render {
inline constexpr core::u64 director_link_undo_context = 0x4c494e4bU;
struct DirectorLinkContext {
    doc::Root document;
    scene::SceneId target{}, program{};
    core::u64 project{};
    bool preview{}, locked{};
};
// Step 12 evidence, recorded on the service worker and published with status.
// Durations are the Director's own boundary: the first byte of a revision's
// transfer through the receipt it sends. Blender evaluation and extraction are
// measured separately by the publisher; the two are never summed into one
// percentile. Percentiles are nearest-rank over at most `sample_capacity`
// successful revisions of this pairing.
struct DirectorLinkMetrics {
    static constexpr std::size_t sample_capacity = 256;
    std::string client, capabilities;
    core::u64 accepted{}, refused{}, resynced{}, transfers{};
    core::u64 peak_pending_bytes{}, retained_resources{}, retained_bytes{}, retained_receipts{};
    core::u64 last_ms{}, median_ms{}, tail_ms{};
    core::u64 last_transfer_ms{}, last_ingest_ms{}, last_admit_ms{}, worst_admit_ms{};
};
struct DirectorLinkStatus {
    bool listening{};
    unsigned port{};
    std::string pairing_code, message;
    bool connected{}, pending{}, failed{};
    std::string source_uuid;
    DirectorLinkMetrics metrics;
};
struct DirectorLinkCandidate {
    LinkedManifest manifest;
    DirectorLinkContext base;
    core::u64 ticket{};
    std::string base_revision, transaction, origin;
    std::stop_token cancellation;
    // A shot transaction shares Director admission, but never replaces source data.
    doc::NodeId shot_node{};
    std::string shot_property, shot_base, shot_edit;
    std::chrono::steady_clock::time_point shot_deadline{};
};
// Owns one bounded worker and one transaction mailbox. Network, source parsing,
// hashing, disk writes and ingestion occur only on that worker. Control-thread
// calls exchange bounded pointers; stop never joins a worker on the frame path.
class DirectorLinkService final {
public:
    DirectorLinkService();
    ~DirectorLinkService();
    void start(std::string cache_directory, DirectorLinkContext context, unsigned port = 0);
    void stop();
    void update(DirectorLinkContext context);
    // Bounded latest-sample overwrite; drops on contention, never blocks a frame.
    void sample(const LinkedStageSample& value) noexcept;
    [[nodiscard]] std::shared_ptr<const DirectorLinkStatus> status();
    [[nodiscard]] std::shared_ptr<const DirectorLinkCandidate> take();
    // A receipt is emitted only after the Director schedules the full document.
    void resolve(core::u64 ticket, std::string rejection = {});
    // Recheck current authoring context and rebase retained shot overrides.
    [[nodiscard]] static scene::LinkedPlanResult plan(const DirectorLinkCandidate& candidate,
        const DirectorLinkContext& current);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mediaserver::render
