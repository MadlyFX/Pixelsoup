#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mediaserver::render {
struct BlenderInstance {
    std::string id, label, scene, status;
    // The open .blend (empty while unsaved), the token Pixelsoup gave the
    // process when it launched it, and the process id used to raise its window.
    std::string file, token;
    unsigned long pid{};
    bool resync_requested{};
    // One Blender holds a project's scenes, each a Blender scene tagged with
    // its Pixelsoup source identity: the scene Blender shows, and every
    // tagged scene in its file.
    std::string active_source;
    std::vector<std::string> scene_sources;
    // The artist asked to add the scene they are in to Pixelsoup.
    std::string create_source, create_name;
};
// A Pixelsoup scene a Blender instance holds, with its pairing.
struct BlenderSceneLink {
    std::string source, name, code;
    unsigned port{};
    bool operator==(const BlenderSceneLink&) const = default;
};
// A Blender start requested with `launch`. `error` is empty once the process
// has been created; whether the add-on reports in is visible as an instance.
struct BlenderLaunchResult {
    std::string token, error;
};
struct BlenderDiscoveryState {
    std::vector<BlenderInstance> instances;
    std::vector<BlenderLaunchResult> launches;
    std::string error;
};
// Same-user discovery and scene lists, serviced off the frame thread. Instance
// records expire; the add-on consumes and deletes the files written to it.
// Starting Blender, finding it on disk and raising its window also happen on the
// worker, never on the frame path.
class BlenderDiscovery final {
public:
    explicit BlenderDiscovery(std::filesystem::path directory = default_directory());
    ~BlenderDiscovery();
    [[nodiscard]] static std::filesystem::path default_directory();
    // MEDIASERVER_BLENDER, then the newest installed Blender 5.2 or later.
    [[nodiscard]] static std::filesystem::path find_executable();
    // integrations/blender beside the build tree, or blender/ in a package:
    // the add-on and the script that opens a scene file with it enabled.
    [[nodiscard]] static std::filesystem::path integration_directory();
    [[nodiscard]] std::shared_ptr<const BlenderDiscoveryState> status() const;
    // Every scene the instance is linked to now, with its pairing, replacing
    // the list it had (empty unlinks it). It syncs whichever of them it shows.
    void link_scenes(std::string id, std::vector<BlenderSceneLink> scenes);
    // Asks the instance to show the scene with `source`, creating it as `name`
    // when its file has none yet. `adopt` instead makes the scene it shows now
    // that scene: the operator linked that Blender to it.
    void activate(std::string id, std::string source, std::string name, bool adopt = false);
    // Opens `file` in a new Blender with the LiveLink add-on enabled, creating
    // it from Blender's startup scene when it does not exist yet. The process
    // advertises `token` (32 lowercase hex) so the caller can recognize it.
    // `scene_source` and `scene_name`, when given, pick the scene it shows.
    void launch(std::string token, std::filesystem::path file,
        std::string scene_source = {}, std::string scene_name = {});
    // Brings the main window of a discovered instance to the front.
    void focus(unsigned long pid);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Exposed for tests: the record format shared with the add-on.
[[nodiscard]] bool parse_blender_instance(const std::string& text, BlenderInstance& instance);
[[nodiscard]] bool valid_link_source(const std::string& value);
}
