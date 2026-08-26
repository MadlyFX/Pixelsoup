#include <mediaserver/persist/recent_project.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define RECENT_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mediaserver-recent-project-" + std::to_string(timestamp) +
             "-" + std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        const bool created =
            std::filesystem::create_directory(path_, error);
        if (!created || error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(
        std::string_view name) const {
        return path_ / std::filesystem::path{name};
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] doc::Root project(std::int64_t value) {
    doc::Node node{};
    node.id = doc::NodeId{42U};
    node.type = "test.project";
    node.properties.emplace("value", value);
    return doc::Root{}.set(std::move(node));
}

[[nodiscard]] bool same_document(const doc::Root& left,
                                 const doc::Root& right) {
    return left.version() == right.version() &&
        doc::diff(left, right).changes.empty();
}

[[nodiscard]] std::vector<std::byte> read_bytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    RECENT_CHECK(input);
    const auto end = input.tellg();
    RECENT_CHECK(end >= 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        RECENT_CHECK(input);
    }
    return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    RECENT_CHECK(output);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    RECENT_CHECK(output);
}

void missing_settings_are_an_empty_normal_startup() {
    TempDirectory temporary;
    const auto settings = temporary.file("settings/recent-project.bin");
    const auto loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(loaded.has_value());
    RECENT_CHECK(!loaded.value().has_value());

    const auto opened = persist::open_recent_project(settings);
    RECENT_CHECK(opened.has_value());
    RECENT_CHECK(!opened.value().has_value());
}

void paths_round_trip_absolutely_and_settings_create_their_directory() {
    TempDirectory temporary;
    const auto settings = temporary.file("nested/settings/recent-project.bin");
    const auto relative = std::filesystem::relative(
        temporary.file("shows/Stage â.mstage"),
        std::filesystem::current_path());

    const auto saved =
        persist::save_recent_project_path(settings, relative);
    RECENT_CHECK(saved.has_value());
    RECENT_CHECK(saved.value().is_absolute());
    RECENT_CHECK(std::filesystem::exists(settings));

    const auto loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(loaded.has_value());
    RECENT_CHECK(loaded.value().has_value());
    RECENT_CHECK(*loaded.value() == saved.value());
}

void replacing_a_setting_is_atomic_and_invalid_input_preserves_it() {
    TempDirectory temporary;
    const auto settings = temporary.file("settings/recent-project.bin");
    const auto first = temporary.file("first.mstage");
    const auto second = temporary.file("second.mstage");
    RECENT_CHECK(persist::save_recent_project_path(settings, first));
    const auto first_bytes = read_bytes(settings);

    const auto invalid =
        persist::save_recent_project_path(settings, std::filesystem::path{});
    RECENT_CHECK(!invalid.has_value());
    RECENT_CHECK(invalid.error().code ==
                 persist::RecentProjectErrorCode::InvalidProjectPath);
    RECENT_CHECK(read_bytes(settings) == first_bytes);

    RECENT_CHECK(persist::save_recent_project_path(settings, second));
    const auto loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(loaded.has_value() && loaded.value().has_value());
    RECENT_CHECK(*loaded.value() ==
                 std::filesystem::absolute(second).lexically_normal());

    std::size_t entries{};
    for (const auto& unused :
         std::filesystem::directory_iterator(settings.parent_path())) {
        static_cast<void>(unused);
        ++entries;
    }
    RECENT_CHECK(entries == 1U);
}

void corruption_truncation_and_trailing_data_are_rejected() {
    TempDirectory temporary;
    const auto settings = temporary.file("recent-project.bin");
    RECENT_CHECK(persist::save_recent_project_path(
        settings, temporary.file("project.mstage")));
    const auto valid = read_bytes(settings);

    auto corrupt = valid;
    corrupt.back() ^= std::byte{0x20U};
    write_bytes(settings, corrupt);
    auto loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(!loaded.has_value());
    RECENT_CHECK(loaded.error().code ==
                 persist::RecentProjectErrorCode::CorruptFile);

    for (std::size_t size = 0U; size < valid.size(); ++size) {
        write_bytes(settings,
                    std::span<const std::byte>{valid}.first(size));
        loaded = persist::load_recent_project_path(settings);
        RECENT_CHECK(!loaded.has_value());
    }

    auto trailing = valid;
    trailing.push_back(std::byte{0U});
    write_bytes(settings, trailing);
    loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(!loaded.has_value());
}

void startup_returns_only_a_fully_valid_existing_project() {
    TempDirectory temporary;
    const auto settings = temporary.file("settings/recent-project.bin");
    const auto project_path = temporary.file("show.mstage");
    const auto expected = project(17);
    RECENT_CHECK(persist::save_project_file(project_path, expected));
    RECENT_CHECK(
        persist::save_recent_project_path(settings, project_path));

    auto opened = persist::open_recent_project(settings);
    RECENT_CHECK(opened.has_value() && opened.value().has_value());
    RECENT_CHECK(opened.value()->path ==
                 std::filesystem::absolute(project_path).lexically_normal());
    RECENT_CHECK(same_document(opened.value()->project, expected));

    std::error_code remove_error;
    RECENT_CHECK(std::filesystem::remove(project_path, remove_error));
    RECENT_CHECK(!remove_error);
    opened = persist::open_recent_project(settings);
    RECENT_CHECK(!opened.has_value());
    RECENT_CHECK(opened.error().project_error.has_value());
    RECENT_CHECK(opened.error().project_error->code ==
                 persist::ProjectFileErrorCode::OpenFailed);
    RECENT_CHECK(!opened.error().settings_error.has_value());

    const std::array garbage{
        std::byte{'n'}, std::byte{'o'}, std::byte{'t'}, std::byte{'p'}};
    write_bytes(project_path, garbage);
    opened = persist::open_recent_project(settings);
    RECENT_CHECK(!opened.has_value());
    RECENT_CHECK(opened.error().project_error.has_value());
    RECENT_CHECK(opened.error().project_error->code ==
                 persist::ProjectFileErrorCode::InvalidFormat ||
                 opened.error().project_error->code ==
                     persist::ProjectFileErrorCode::CorruptFile);
}

void the_default_location_is_per_user_and_outside_the_project() {
    const auto located = persist::default_recent_project_settings_file();
    // Sandboxed/service accounts may intentionally expose no user profile.
    // That is the documented nonfatal startup fallback, not a test failure.
    if (!located) {
        RECENT_CHECK(located.error().code ==
                     persist::RecentProjectErrorCode::SettingsUnavailable);
        return;
    }
    RECENT_CHECK(located.has_value());
    RECENT_CHECK(located.value().is_absolute());
    RECENT_CHECK(located.value().filename() == "recent-project.bin");
    RECENT_CHECK(located.value().parent_path().filename() == "settings");
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"missing settings are a normal startup",
     &missing_settings_are_an_empty_normal_startup},
    {"paths round trip and create the settings directory",
     &paths_round_trip_absolutely_and_settings_create_their_directory},
    {"replacement is atomic and invalid input preserves it",
     &replacing_a_setting_is_atomic_and_invalid_input_preserves_it},
    {"corruption, truncation, and trailing data are rejected",
     &corruption_truncation_and_trailing_data_are_rejected},
    {"startup returns only a valid existing project",
     &startup_returns_only_a_fully_valid_existing_project},
    {"the default location is per-user",
     &the_default_location_is_per_user_and_outside_the_project},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto& entry : cases) {
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
