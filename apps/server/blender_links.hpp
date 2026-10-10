#pragma once

#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/render/director_link_service.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::app {

// A scene's saved association with a Blender file, read from its stage node.
// `file` and `source` are empty until the scene is first opened or connected.
struct BlenderAssociation final {
    doc::NodeId scene{};
    std::string name, file, source;
};

// The "<project> Blender" folder beside a saved project, else Media
// Server/Blender under the user's Documents: where a project's Blender files
// and the content Blender sends it live.
[[nodiscard]] std::filesystem::path blender_project_folder(const std::string& project_path);
// The content-addressed store linked geometry is ingested into. Scenes refer
// to it by absolute path, and it sits beside the project, so every output
// node that opens the project from a share reads the same files.
[[nodiscard]] std::filesystem::path link_resource_directory(const std::string& project_path);
// The project's .blend: one Blender file holds a Blender scene for each of the
// project's scenes. "<project>.blend" in the project's Blender folder; an
// unsaved project uses `unsaved_name`.blend under Documents.
[[nodiscard]] std::filesystem::path project_blender_file(
    const std::string& project_path, const std::string& unsaved_name);
// The .blend a scene opening in Blender for the first time joins: the file the
// project's other scenes already use (the most of them), else `fallback`.
[[nodiscard]] std::string shared_blender_file(std::span<const BlenderAssociation> associations,
    doc::NodeId scene, const std::string& fallback);
// Where Publish stages Program files when the operator has not chosen a shared
// folder: the saved project's own folder (every node that opens the project
// from a share sees it at the same path), else Documents/Media Server.
[[nodiscard]] std::string default_publication_storage(const std::string& project_path);
// Brings `associations` (every scene: name, Blender file, link source) from
// `before` to `after` by visiting only the nodes that changed between them.
void refresh_blender_associations(std::vector<BlenderAssociation>& associations,
    const doc::Root& before, const doc::Root& after);
// Path equality as the file system sees it (case-insensitive on Windows).
[[nodiscard]] bool same_blender_file(const std::string& left, const std::string& right);
[[nodiscard]] std::string new_link_source();
[[nodiscard]] std::string path_utf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path utf8_path(const std::string& text);
// Negotiated capabilities and measured latency for the Publish card.
[[nodiscard]] std::string link_diagnostics(const render::DirectorLinkStatus& status);

// Every Blender link of the open project: one Director link service per linked
// scene, so an operator can switch Preview and Program between scenes without
// re-pairing. Each link edits only its own scene, wherever that scene is, so a
// scene on air follows Blender live.
//
// One Blender holds the project: its file has a Blender scene per linked Media
// Server scene, and every one of them is linked to that Blender at once. It
// syncs the scene it shows; switching scenes there switches which one syncs.
// Everything here runs on the control thread and only exchanges bounded state
// with the services' and discovery's workers.
class BlenderLinks final {
public:
    using ContextFor = std::function<render::DirectorLinkContext(doc::NodeId)>;
    struct Candidate final {
        doc::NodeId scene{};
        std::shared_ptr<const render::DirectorLinkCandidate> candidate;
    };
    // `running`: the Blender holding this scene is open. `connected`: it is
    // syncing this scene - the scene it shows.
    struct View final {
        doc::NodeId scene{};
        bool running{}, opening{}, connected{}, pending{}, failed{};
        std::string status;
    };
    // A scene the artist added in Blender, for Media Server to create.
    struct Created final {
        std::string source, name, file;
    };
    struct Moved final {
        doc::NodeId scene{};
        std::string file;
    };
    static constexpr std::size_t max_links = 8;

    explicit BlenderLinks(std::string cache_directory,
        std::filesystem::path discovery_directory = render::BlenderDiscovery::default_directory());
    ~BlenderLinks();
    BlenderLinks(const BlenderLinks&) = delete;
    BlenderLinks& operator=(const BlenderLinks&) = delete;

    // Where services started from now on store what Blender sends. Links
    // already running keep the store they started with.
    void set_cache_directory(std::string directory) { cache_ = std::move(directory); }
    // Brings forward the Blender holding the scene and switches it to that
    // scene, links an open Blender with the scene's file, or starts Blender on
    // the file (creating the file, or the scene in it, the first time).
    void open(const BlenderAssociation& scene, const render::DirectorLinkContext& context);
    // Links a discovered instance to a scene and shows it there. `adopt` makes
    // the scene Blender shows now that scene (the Connect dialog); otherwise
    // Blender finds the scene's Blender scene, or creates it.
    void bind(const std::string& instance, const BlenderAssociation& scene,
        const render::DirectorLinkContext& context, bool adopt = false);
    // Restarts an instance's link with a fresh session and a full snapshot.
    // Returns false when the instance is not linked to any scene.
    bool resync(const std::string& instance, const ContextFor& context_for);
    void disconnect_scene(doc::NodeId scene);
    void disconnect_instance(const std::string& instance);
    void stop_all();

    // Once per control frame, before candidates are taken. `associations`
    // lists the scenes that have a Blender file, for automatic relinking.
    void poll(std::span<const BlenderAssociation> associations, const ContextFor& context_for);
    // The next revision any link has ready, skipping scenes `waiting` names:
    // their revisions stay with their links until a later call.
    [[nodiscard]] std::optional<Candidate> take(const std::function<bool(doc::NodeId)>& waiting = {});
    void resolve(doc::NodeId scene, core::u64 ticket, std::string rejection,
        const render::DirectorLinkContext& context);
    void sample(const render::LinkedStageSample& sample) noexcept;

    [[nodiscard]] std::shared_ptr<const render::BlenderDiscoveryState> discovery() const;
    [[nodiscard]] std::shared_ptr<const render::DirectorLinkStatus> status_for_source(const std::string& uuid) const;
    [[nodiscard]] std::shared_ptr<const render::DirectorLinkStatus> status_for_scene(doc::NodeId scene) const;
    [[nodiscard]] std::string instance_for_scene(doc::NodeId scene) const;
    [[nodiscard]] std::vector<View> views() const;
    // (instance id, scene) for every live link, for the Connect dialog.
    [[nodiscard]] std::vector<std::pair<std::string, doc::NodeId>> bound_instances() const;
    [[nodiscard]] bool any_connected() const noexcept;

    // Operator-facing outcome of the last action or event, once.
    [[nodiscard]] std::string take_message();
    // Links whose Director status changed since the last call.
    [[nodiscard]] std::vector<std::pair<doc::NodeId, std::shared_ptr<const render::DirectorLinkStatus>>>
    take_status_changes();
    // A linked Blender saved its scene under a new name; the scene follows it.
    [[nodiscard]] std::vector<Moved> take_moved();
    // Scenes the artist added to Media Server from Blender, once each.
    [[nodiscard]] std::vector<Created> take_created();

private:
    struct Slot;
    struct Launch;
    Slot* slot_for(doc::NodeId scene, bool create);
    void start_service(Slot& slot, const render::DirectorLinkContext& context);
    void publish_links(std::chrono::steady_clock::time_point now);
    [[nodiscard]] const render::BlenderInstance* instance(const std::string& id) const;
    [[nodiscard]] bool launching(doc::NodeId scene) const;

    std::string cache_;
    render::BlenderDiscovery discovery_;
    std::shared_ptr<const render::BlenderDiscoveryState> state_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<Launch> launches_;
    std::string message_;
    std::vector<Moved> moved_;
    std::vector<Created> created_;
    std::vector<std::string> requested_sources_;
    // The scene list last sent to each Blender.
    struct Published final {
        std::vector<render::BlenderSceneLink> scenes;
        std::chrono::steady_clock::time_point at{};
    };
    std::map<std::string, Published> published_;
    std::vector<std::pair<doc::NodeId, std::shared_ptr<const render::DirectorLinkStatus>>> changed_;
    std::size_t next_take_{};
};

} // namespace mediaserver::app
