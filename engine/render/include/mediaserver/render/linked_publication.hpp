#pragma once
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <memory>
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
    // verified rollback revision using the same staging path.
    std::shared_ptr<const PublicationResult> retained;
    doc::NodeId scene{};
    std::string manifest, revision, diagnostic;
    core::u64 document_revision{}, bytes{}, texture_bytes{}, vertices{}, indices{};
};
// Worker-only entry point, also used by the qualification harness.
[[nodiscard]] PublicationResult prepare_linked_publication(const PublicationRequest& request,
    std::stop_token stop = {});
// Adds a frozen scene and its compositor as one Director transaction. IDs are
// allocated against the current document, so Preview may change while staging.
[[nodiscard]] scene::LinkedPlanResult install_linked_publication(const doc::Root& current,
    const PublicationResult& prepared, doc::NodeId program, doc::NodeId first);
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
