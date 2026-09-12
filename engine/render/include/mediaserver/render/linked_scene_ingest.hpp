#pragma once
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace mediaserver::render {
// Portable manifest identities never contain cache paths. The resolved package
// location is a local renderer fallback, separate from link_resource identity.
struct LinkedManifestRecord {
    doc::Node node;
    std::string parent, prototype, material, payload;
    std::string lod_payload, lod_hash;
    float lod_pixels{};
};
struct LinkedManifest {
    std::string source_uuid, revision;
    std::vector<LinkedManifestRecord> records;
    std::size_t ingested{}, reused{};
    std::string collection_name;
};
struct LinkedIngestResult {
    LinkedManifest manifest;
    std::string diagnostic;
};
// Worker-only. Limits: 1 MiB manifest, 1024 records, 64 prototypes,
// 64 MiB per embedded GLB and 256 MiB total input. Cache is immutable on disk.
[[nodiscard]] LinkedIngestResult ingest_linked_scene(const std::string& path,
    std::stop_token stop = {});
[[nodiscard]] scene::LinkedPlanResult plan_linked_manifest(const doc::Root& root,
    const LinkedManifest& manifest, scene::SceneId target);
// One active worker, one latest pending request. Superseded work cannot publish.
class LinkedSceneIngest final {
public:
    LinkedSceneIngest();
    ~LinkedSceneIngest();
    void submit(std::string path);
    void cancel();
    [[nodiscard]] std::shared_ptr<const LinkedIngestResult> take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mediaserver::render
