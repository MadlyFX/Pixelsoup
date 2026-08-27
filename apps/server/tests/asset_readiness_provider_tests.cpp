#include "asset_readiness_provider.hpp"

#include <mediaserver/assets/asset_path.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace net = mediaserver::net;
namespace fs = std::filesystem;

#define READINESS_CHECK(condition)                                          \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};          \
        }                                                                   \
    } while (false)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_readiness_" + std::to_string(stamp));
        std::error_code error;
        if (!fs::create_directory(path_, error)) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write test asset"};
    }
}

void a_present_file_is_ready_and_a_missing_one_is_missing() {
    TemporaryDirectory temporary;
    const fs::path present = temporary.path() / "clip.mp4";
    write(present, "frames");
    const fs::path absent = temporary.path() / "gone.mp4";

    app::PathAssetReadinessProvider provider;
    const auto ready = provider.probe(assets::asset_source_from_path(present));
    READINESS_CHECK(ready.has_value());
    READINESS_CHECK(*ready == net::ClusterAssetReadinessState::Ready);

    const auto missing = provider.probe(assets::asset_source_from_path(absent));
    READINESS_CHECK(missing.has_value());
    READINESS_CHECK(*missing == net::ClusterAssetReadinessState::Missing);
}

void a_directory_at_the_authored_path_is_not_ready() {
    TemporaryDirectory temporary;
    // A decoder cannot open a directory, so reporting Ready here would let an
    // operator promote a look that renders nothing.
    const auto state =
        app::PathAssetReadinessProvider{}.probe(
            assets::asset_source_from_path(temporary.path()));
    READINESS_CHECK(state.has_value());
    READINESS_CHECK(*state == net::ClusterAssetReadinessState::Missing);
}

void an_unusable_request_is_a_provider_failure_not_a_verdict() {
    app::PathAssetReadinessProvider provider;
    // A relative source and an over-long one are both malformed requests. The
    // agent must not answer Missing, which would read as an ordinary
    // observation about a real path.
    READINESS_CHECK(!provider.probe("clip.mp4").has_value());
    const std::string too_long(net::max_cluster_asset_source_bytes + 1U, 'a');
    READINESS_CHECK(!provider.probe(too_long).has_value());

    std::stop_source stopped;
    stopped.request_stop();
    READINESS_CHECK(
        !provider.probe("clip.mp4", stopped.get_token()).has_value());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"a present file is ready and a missing one is missing",
     &a_present_file_is_ready_and_a_missing_one_is_missing},
    {"a directory at the authored path is not ready",
     &a_directory_at_the_authored_path_is_not_ready},
    {"an unusable request is a provider failure not a verdict",
     &an_unusable_request_is_a_provider_failure_not_a_verdict},
};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    const std::size_t total = std::size(cases);
    std::cout << (total - failures) << '/' << total << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
