#include <mediaserver/render/blender_discovery.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace mediaserver::render {
namespace fs = std::filesystem;
namespace {
bool hex(const std::string& value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool fresh(const fs::directory_entry& entry) {
    const auto age = fs::file_time_type::clock::now() - entry.last_write_time();
    return !entry.is_symlink() && entry.is_regular_file() && entry.file_size() <= 2048
        && age >= -std::chrono::seconds{2} && age < std::chrono::seconds{10};
}
}
struct BlenderDiscovery::Impl {
    struct Request { std::string id, code; unsigned port{}; };
    fs::path directory;
    std::atomic<std::shared_ptr<const BlenderDiscoveryState>> state;
    std::atomic<std::shared_ptr<const Request>> request, disconnect_request;
    std::jthread worker;
    explicit Impl(fs::path root) : directory{std::move(root)},
        worker{[this](std::stop_token stop) { run(stop); }} {}
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto next = std::make_shared<BlenderDiscoveryState>();
            try {
                if (fs::exists(directory)) {
                    std::size_t visited{};
                    for (const auto& entry : fs::directory_iterator{directory}) {
                        if (++visited > 512) break;
                        if (entry.path().extension() != ".instance" || !fresh(entry)) continue;
                        BlenderInstance item;
                        item.id = entry.path().stem().string();
                        if (!hex(item.id, 32)) continue;
                        std::ifstream input{entry.path()};
                        std::string magic;
                        if (std::getline(input, magic) && magic == "MSBLENDER 1"
                            && std::getline(input, item.label) && std::getline(input, item.scene)
                            && std::getline(input, item.status)) {
                            std::string request_kind;
                            if (std::getline(input, request_kind)) item.resync_requested = request_kind == "resync";
                            next->instances.push_back(std::move(item));
                        }
                        if (next->instances.size() >= 64) break;
                    }
                }
                std::sort(next->instances.begin(), next->instances.end(), [](const auto& a, const auto& b) {
                    return a.label != b.label ? a.label < b.label : a.id < b.id;
                });
                for (const auto& command : {disconnect_request.exchange({}), request.exchange({})}) {
                    if (!command) continue;
                    const auto found = std::find_if(next->instances.begin(), next->instances.end(),
                        [&](const auto& item) { return item.id == command->id; });
                    if (found == next->instances.end()) next->error = "Blender is no longer available. Select an available instance.";
                    else {
                        const auto temporary = directory / (command->id + ".request.tmp");
                        const auto target = directory / (command->id + ".request");
                        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
                        output << "MSCONNECT 1\n";
                        if (command->port) output << "connect\n" << command->port << '\n' << command->code << '\n';
                        else output << "disconnect\n";
                        output.close();
                        if (!output) throw std::runtime_error{"Cannot send the Blender connection request"};
                        fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
                        // A new user action supersedes any unconsumed request.
                        fs::remove(target);
                        fs::rename(temporary, target);
                    }
                }
            } catch (const std::exception& error) { next->error = error.what(); }
            state.store(std::move(next));
            for (int tick = 0; tick < 10 && !stop.stop_requested() && !request.load() && !disconnect_request.load(); ++tick)
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }
};
fs::path BlenderDiscovery::default_directory() {
#ifdef _WIN32
    char* base{}; std::size_t length{};
    static_cast<void>(_dupenv_s(&base, &length, "LOCALAPPDATA"));
    const auto path = (base ? fs::path{base} : fs::temp_directory_path()) / "MediaServer" / "blender";
    std::free(base);
    return path;
#else
    const auto* base = std::getenv("HOME");
    return (base ? fs::path{base} / ".cache" : fs::temp_directory_path()) / "MediaServer" / "blender";
#endif
}
BlenderDiscovery::BlenderDiscovery(fs::path directory) : impl_{std::make_unique<Impl>(std::move(directory))} {}
BlenderDiscovery::~BlenderDiscovery() = default;
std::shared_ptr<const BlenderDiscoveryState> BlenderDiscovery::status() const { return impl_->state.load(); }
void BlenderDiscovery::connect(std::string id, unsigned port, std::string code) {
    if (hex(id, 32) && port >= 1024 && port <= 65535 && hex(code, 64))
        impl_->request.store(std::make_shared<Impl::Request>(std::move(id), std::move(code), port));
}
void BlenderDiscovery::disconnect(std::string id) {
    if (hex(id, 32)) impl_->disconnect_request.store(std::make_shared<Impl::Request>(std::move(id), std::string{}, 0));
}
}
