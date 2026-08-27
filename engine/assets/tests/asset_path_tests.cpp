#include <mediaserver/assets/asset_path.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

namespace assets = mediaserver::assets;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PATH_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A directory that removes itself, so a failing check cannot leave temporary
// files behind for the next run to trip over.
class ScopedDirectory final {
  public:
    ScopedDirectory() {
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) {
            throw std::runtime_error{"no temp directory"};
        }
        path_ = base / "mediaserver-asset-path-tests";
        std::filesystem::remove_all(path_, error);
        if (!std::filesystem::create_directories(path_, error) || error) {
            throw std::runtime_error{"could not create temp directory"};
        }
    }

    ~ScopedDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ScopedDirectory(const ScopedDirectory&) = delete;
    ScopedDirectory& operator=(const ScopedDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void relative_sources_are_refused() {
    // Every one of these would resolve against a process working directory,
    // which differs between the editor and a lifecycle-supervised child.
    PATH_CHECK(!assets::valid_asset_source(""));
    PATH_CHECK(!assets::valid_asset_source("clip.mov"));
    PATH_CHECK(!assets::valid_asset_source("media/clip.mov"));
    PATH_CHECK(!assets::valid_asset_source("./clip.mov"));
    PATH_CHECK(!assets::valid_asset_source("../media/clip.mov"));
    PATH_CHECK(!assets::valid_asset_source(std::string{"D:\\media\0x", 10U}));
}

void dot_components_are_refused_inside_an_absolute_path() {
    // An absolute path is not enough: a '..' anywhere still lets two machines
    // that normalise differently disagree about the same authored string.
#if defined(_WIN32)
    PATH_CHECK(!assets::valid_asset_source("D:\\media\\..\\media\\clip.mov"));
    PATH_CHECK(!assets::valid_asset_source("D:\\media\\.\\clip.mov"));
#else
    PATH_CHECK(!assets::valid_asset_source("/media/../media/clip.mov"));
    PATH_CHECK(!assets::valid_asset_source("/media/./clip.mov"));
#endif
}

void absolute_sources_are_accepted_without_touching_the_filesystem() {
    // A share that is not mounted yet is still a valid authored source; only
    // the readiness report may say the file is absent.
#if defined(_WIN32)
    PATH_CHECK(assets::valid_asset_source("D:\\media\\clip.mov"));
    PATH_CHECK(assets::valid_asset_source("\\\\wall\\media\\clip.mov"));
    PATH_CHECK(!assets::asset_source_present(
        "D:\\media\\definitely-not-here-9c1f.mov"));
#else
    PATH_CHECK(assets::valid_asset_source("/media/clip.mov"));
    PATH_CHECK(!assets::asset_source_present("/media/definitely-not-here.mov"));
#endif
}

void presence_distinguishes_a_file_from_a_directory() {
    const ScopedDirectory temporary;
    const auto file = temporary.path() / "clip.mov";
    {
        std::ofstream stream{file, std::ios::binary};
        stream << "frames";
    }

    const std::string file_source = assets::asset_source_from_path(file);
    const std::string directory_source =
        assets::asset_source_from_path(temporary.path());

    PATH_CHECK(assets::valid_asset_source(file_source));
    PATH_CHECK(assets::asset_source_present(file_source));
    // A directory at the authored path is not something a decoder can open.
    PATH_CHECK(!assets::asset_source_present(directory_source));

    std::error_code error;
    std::filesystem::remove(file, error);
    PATH_CHECK(!assets::asset_source_present(file_source));
}

void source_text_round_trips_through_a_native_path() {
    const ScopedDirectory temporary;
    const auto file = temporary.path() / "clip-\u00e9\u00fc.mov";
    {
        std::ofstream stream{file, std::ios::binary};
        stream << "frames";
    }
    const std::string source = assets::asset_source_from_path(file);
    PATH_CHECK(assets::asset_path_from_source(source) == file);
    PATH_CHECK(assets::asset_source_present(source));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"relative sources are refused", &relative_sources_are_refused},
    {"dot components are refused inside an absolute path",
     &dot_components_are_refused_inside_an_absolute_path},
    {"absolute sources are accepted without touching the filesystem",
     &absolute_sources_are_accepted_without_touching_the_filesystem},
    {"presence distinguishes a file from a directory",
     &presence_distinguishes_a_file_from_a_directory},
    {"source text round trips through a native path",
     &source_text_round_trips_through_a_native_path},
}};

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
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
