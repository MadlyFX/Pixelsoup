#pragma once
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <memory>
#include <span>
#include <stop_token>

namespace mediaserver::render {
struct PublicationLimits {
    core::u64 bytes{512ULL * 1024 * 1024};
    core::u64 texture_bytes{256ULL * 1024 * 1024};
    core::u64 vertices{1ULL << 20}, indices{3ULL << 20};
    std::size_t resources{128}, nodes{4096};
};
struct PublicationRequest {
    doc::Root document;
    doc::NodeId scene{}, program{};
    std::string storage;
    // Verification reads an already staged manifest; staging freezes a snapshot.
    bool verify{};
    PublicationLimits limits{};
};
struct PublicationResult {
    doc::Root document;
    // On the first take, capture an ordinary Program as an independent,
    // verified rollback revision using the same staging path. A Program that
    // cannot be frozen (an image sequence, an unpackaged model) does not block
    // the publication: rollback then returns to the live Program scene and
    // `notice` says why.
    std::shared_ptr<const PublicationResult> retained;
    doc::NodeId scene{};
    std::string manifest, revision, diagnostic, notice;
    core::u64 document_revision{}, bytes{}, texture_bytes{}, vertices{}, indices{};
};
// Worker-only entry point, also used by the qualification harness.
[[nodiscard]] PublicationResult prepare_linked_publication(const PublicationRequest& request,
    std::stop_token stop = {});
// Adds a frozen scene and its compositor as one Director transaction. IDs are
// allocated against the current document, so Preview may change while staging.
[[nodiscard]] scene::LinkedPlanResult install_linked_publication(const doc::Root& current,
    const PublicationResult& prepared, doc::NodeId program, doc::NodeId first);
// Removes published revisions nothing can return to: everything except
// Program, its rollback, a pending candidate and that candidate's retained
// previous Program, and any scene in `in_use` (the switcher's). Without this,
// every PUBLISH adds a scene until the 16-revision history refuses the next.
[[nodiscard]] doc::Root prune_publications(const doc::Root& current, doc::NodeId program,
    std::span<const doc::NodeId> in_use = {});
// One bounded worker, one latest request, cancellation fences every completion.
class LinkedPublication final {
public:
    LinkedPublication();
    ~LinkedPublication();
    void submit(PublicationRequest request);
    void cancel();
    [[nodiscard]] std::shared_ptr<const PublicationResult> take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Each output process verifies a revision once, on its own worker. A failure
// stays failed until a fresh candidate/restart; there is no periodic disk work.
class PublicationVerification final {
public:
    [[nodiscard]] bool observe(const doc::Root& document, doc::NodeId scene);
    [[nodiscard]] std::string_view diagnostic() const noexcept { return diagnostic_; }
private:
    LinkedPublication worker_;
    doc::NodeId scene_{};
    std::string manifest_, manifest_hash_, revision_, diagnostic_;
    bool ready_{true};
};
} // namespace mediaserver::render
