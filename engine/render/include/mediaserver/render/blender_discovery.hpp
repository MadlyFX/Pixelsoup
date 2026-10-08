#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mediaserver::render {
struct BlenderInstance {
    std::string id, label, scene, status;
    bool resync_requested{};
};
struct BlenderDiscoveryState {
    std::vector<BlenderInstance> instances;
    std::string error;
};
// Same-user discovery and ephemeral connection requests, serviced off the frame
// thread. Instance records expire; the add-on consumes and deletes credentials.
class BlenderDiscovery final {
public:
    explicit BlenderDiscovery(std::filesystem::path directory = default_directory());
    ~BlenderDiscovery();
    [[nodiscard]] static std::filesystem::path default_directory();
    [[nodiscard]] std::shared_ptr<const BlenderDiscoveryState> status() const;
    void connect(std::string id, unsigned port, std::string code);
    void disconnect(std::string id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
