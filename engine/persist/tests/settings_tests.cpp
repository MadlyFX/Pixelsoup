#include <mediaserver/persist/settings.hpp>

#include <algorithm>
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

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define RECENT_CHECK(...)                                                      \
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
        const bool created = std::filesystem::create_directory(path_, error);
        if (!created || error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / std::filesystem::path{name};
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
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

[[nodiscard]] bool same_document(const doc::Root &left,
                                 const doc::Root &right) {
    return left.version() == right.version() &&
           doc::diff(left, right).changes.empty();
}

[[nodiscard]] std::vector<std::byte>
read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    RECENT_CHECK(input);
    const auto end = input.tellg();
    RECENT_CHECK(end >= 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        RECENT_CHECK(input);
    }
    return bytes;
}

void write_bytes(const std::filesystem::path &path,
                 std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    RECENT_CHECK(output);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char *>(bytes.data()),
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
    const auto project_path = temporary.file("shows/Stage â.mstage");
    const auto relative = std::filesystem::relative(
        project_path, std::filesystem::current_path());
    // Windows cannot express a relative path across drives.
    const auto input = relative.empty() ? project_path : relative;

    const auto saved = persist::save_recent_project_path(settings, input);
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
                 persist::SettingsErrorCode::InvalidProjectPath);
    RECENT_CHECK(read_bytes(settings) == first_bytes);

    RECENT_CHECK(persist::save_recent_project_path(settings, second));
    const auto loaded = persist::load_recent_project_path(settings);
    RECENT_CHECK(loaded.has_value() && loaded.value().has_value());
    RECENT_CHECK(*loaded.value() ==
                 std::filesystem::absolute(second).lexically_normal());

    std::size_t entries{};
    for (const auto &unused :
         std::filesystem::directory_iterator(settings.parent_path())) {
        static_cast<void>(unused);
        ++entries;
    }
    RECENT_CHECK(entries == 1U);
}

void recent_projects_are_bounded_unique_and_newest_first() {
    TempDirectory temporary;
    const auto settings = temporary.file("settings/recent-project.bin");
    std::vector<std::filesystem::path> projects;
    for (std::size_t index = 0U; index < persist::max_recent_projects + 2U;
         ++index) {
        const auto path = temporary.file("show-" + std::to_string(index) + ".mstage");
        projects.push_back(path);
        RECENT_CHECK(persist::save_recent_project_path(settings, path));
    }
    auto loaded = persist::load_recent_project_paths(settings);
    RECENT_CHECK(loaded.has_value());
    RECENT_CHECK(loaded.value().size() == persist::max_recent_projects);
    for (std::size_t index = 0U; index < persist::max_recent_projects; ++index) {
        RECENT_CHECK(loaded.value()[index] == projects[projects.size() - 1U - index]);
    }
    RECENT_CHECK(persist::save_recent_project_path(settings, projects[3U]));
    loaded = persist::load_recent_project_paths(settings);
    RECENT_CHECK(loaded.has_value());
    RECENT_CHECK(loaded.value().size() == persist::max_recent_projects);
    RECENT_CHECK(loaded.value().front() == projects[3U]);
    RECENT_CHECK(std::count(loaded.value().begin(), loaded.value().end(),
                            projects[3U]) == 1);
    const auto newest = persist::load_recent_project_path(settings);
    RECENT_CHECK(newest.has_value() && newest.value() == projects[3U]);
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
                 persist::SettingsErrorCode::CorruptFile);

    for (std::size_t size = 0U; size < valid.size(); ++size) {
        write_bytes(settings, std::span<const std::byte>{valid}.first(size));
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
    RECENT_CHECK(persist::save_recent_project_path(settings, project_path));

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

    const std::array garbage{std::byte{'n'}, std::byte{'o'}, std::byte{'t'},
                             std::byte{'p'}};
    write_bytes(project_path, garbage);
    opened = persist::open_recent_project(settings);
    RECENT_CHECK(!opened.has_value());
    RECENT_CHECK(opened.error().project_error.has_value());
    RECENT_CHECK(opened.error().project_error->code ==
                     persist::ProjectFileErrorCode::InvalidFormat ||
                 opened.error().project_error->code ==
                     persist::ProjectFileErrorCode::CorruptFile);
}

[[nodiscard]] persist::Preferences sample_preferences() {
    persist::Preferences preferences{};
    preferences.director_listen = "10.20.0.10:45000";
    preferences.show_id = "0000000000000001000000000000000B";
    preferences.director_epoch = "0000000000000003000000000000000C";
    preferences.lifecycle_password_key =
        "c524ac566c0a43e0355b43cc74ea5d972b1331f26cb8211794ccec2187694830";
    // Written as UTF-8 bytes so the source stays representable in any
    // compiler code page while still exercising a non-ASCII path.
    preferences.wal_path = "D:\\shows\\"
                           "\xe6\x9d\xb1"
                           "\xe4\xba\xac"
                           ".wal";
    preferences.proxy_cache_directory =
        "D:\\cache\\"
        "\xe3\x83\x97\xe3\x83\xad\xe3\x82\xad\xe3\x82\xb7";
    preferences.proxy_cache_next_to_source = true;
    preferences.proxy_cache_gibibytes = 64U;
    preferences.output_buffer_frames = 24U;
    preferences.output_read_ahead_frames = 20U;
    preferences.playback_cache_mebibytes = 4'096U;
    preferences.playback_trail_frames = 6U;
    preferences.proxy_transcode_priority =
        persist::ProxyTranscodePriority::Balanced;
    preferences.render_machine_director = "10.20.0.10:45060";
    preferences.render_machine_node_index = 3U;
    preferences.render_machine_control_port = 47'900U;
    preferences.render_machine_show_id = "0000000000000001000000000000000B";
    return preferences;
}

void unconfigured_preferences_are_empty_not_an_error() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");
    // First run has no file. That must not read as a failure, or the editor
    // would report a problem to every new operator.
    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value().empty());
    RECENT_CHECK(!std::filesystem::exists(file));
}

void preferences_round_trip_including_non_ascii_paths() {
    const TempDirectory temporary;
    const auto file = temporary.file("nested/preferences.bin");
    const auto original = sample_preferences();
    const auto saved = persist::save_preferences(file, original);
    RECENT_CHECK(saved);
    RECENT_CHECK(std::filesystem::exists(file));

    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value() == original);
}

void an_empty_field_means_no_preference_and_survives_a_round_trip() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");
    persist::Preferences partial{};
    partial.director_listen = "0.0.0.0:45060";
    RECENT_CHECK(persist::save_preferences(file, partial));

    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value() == partial);
    RECENT_CHECK(loaded.value().wal_path.empty());
    RECENT_CHECK(loaded.value().lifecycle_password_key.empty());

    // Clearing every field is a real choice: it restores the built-in
    // defaults, and it must persist rather than being read as corruption.
    RECENT_CHECK(persist::save_preferences(file, persist::Preferences{}));
    const auto cleared = persist::load_preferences(file);
    RECENT_CHECK(cleared);
    RECENT_CHECK(cleared.value().empty());
}

void a_saved_preference_replaces_the_previous_one_atomically() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");
    RECENT_CHECK(persist::save_preferences(file, sample_preferences()));

    persist::Preferences oversized{};
    oversized.wal_path =
        std::string(persist::max_preference_value_bytes + 1U, 'w');
    const auto rejected = persist::save_preferences(file, oversized);
    RECENT_CHECK(!rejected);
    RECENT_CHECK(rejected.error().code ==
                 persist::SettingsErrorCode::SizeLimitExceeded);

    // A refused save must leave the installed settings untouched.
    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value() == sample_preferences());
}

void invalid_playback_tuning_is_refused_without_replacing_settings() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");
    const auto original = sample_preferences();
    RECENT_CHECK(persist::save_preferences(file, original));

    auto invalid = original;
    invalid.output_read_ahead_frames = 0U;
    const auto rejected = persist::save_preferences(file, invalid);
    RECENT_CHECK(!rejected);
    RECENT_CHECK(rejected.error().code ==
                 persist::SettingsErrorCode::InvalidFormat);

    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value() == original);
}

void render_machine_settings_round_trip_beside_the_editor_settings() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");

    // A render machine that has only ever been set up through its own window:
    // the director and node index are all it stores, and node 0 is a real
    // choice that has to survive even though it is also the default.
    persist::Preferences render_only{};
    render_only.render_machine_director = "192.168.1.20:45060";
    RECENT_CHECK(!render_only.empty());
    RECENT_CHECK(persist::save_preferences(file, render_only));
    const auto loaded = persist::load_preferences(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(loaded.value() == render_only);
    RECENT_CHECK(loaded.value().render_machine_node_index == 0U);
    RECENT_CHECK(loaded.value().render_machine_control_port == 0U);
    RECENT_CHECK(loaded.value().render_machine_show_id.empty());
    RECENT_CHECK(loaded.value().director_listen.empty());

    // The editor saving its own fields onto that record keeps the render
    // machine's: each writer changes only what it owns.
    auto edited = loaded.value();
    edited.director_listen = "0.0.0.0:45060";
    edited.output_buffer_frames = 30U;
    RECENT_CHECK(persist::save_preferences(file, edited));
    const auto reloaded = persist::load_preferences(file);
    RECENT_CHECK(reloaded);
    RECENT_CHECK(reloaded.value().render_machine_director ==
                 "192.168.1.20:45060");
    RECENT_CHECK(reloaded.value().director_listen == "0.0.0.0:45060");

    // A node index the agent would refuse is refused here too, and the
    // installed record is left as it was.
    auto out_of_range = reloaded.value();
    out_of_range.render_machine_node_index =
        persist::maximum_render_machine_node_index + 1U;
    const auto rejected_index = persist::save_preferences(file, out_of_range);
    RECENT_CHECK(!rejected_index);
    RECENT_CHECK(rejected_index.error().code ==
                 persist::SettingsErrorCode::InvalidFormat);
    auto bad_port = reloaded.value();
    bad_port.render_machine_control_port = 65'536U;
    RECENT_CHECK(!persist::save_preferences(file, bad_port));
    const auto kept = persist::load_preferences(file);
    RECENT_CHECK(kept);
    RECENT_CHECK(kept.value() == reloaded.value());

    // Forgetting the render machine is clearing its fields; what is left is
    // the editor's alone.
    auto forgotten = kept.value();
    forgotten.render_machine_director.clear();
    forgotten.render_machine_node_index = 0U;
    RECENT_CHECK(persist::save_preferences(file, forgotten));
    const auto after = persist::load_preferences(file);
    RECENT_CHECK(after);
    RECENT_CHECK(after.value().render_machine_director.empty());
    RECENT_CHECK(after.value().director_listen == "0.0.0.0:45060");
}

void damaged_preferences_are_reported_rather_than_silently_defaulted() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");
    RECENT_CHECK(persist::save_preferences(file, sample_preferences()));

    std::vector<std::byte> bytes;
    {
        std::ifstream input{file, std::ios::binary | std::ios::ate};
        RECENT_CHECK(static_cast<bool>(input));
        bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    const auto rewrite = [&file](std::span<const std::byte> replacement) {
        std::ofstream output{file, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char *>(replacement.data()),
                     static_cast<std::streamsize>(replacement.size()));
    };

    // A flipped payload byte fails the checksum. Running on defaults the
    // operator did not choose would be worse than saying so.
    auto flipped = bytes;
    flipped.back() ^= std::byte{0xffU};
    rewrite(flipped);
    const auto corrupt = persist::load_preferences(file);
    RECENT_CHECK(!corrupt);
    RECENT_CHECK(corrupt.error().code ==
                 persist::SettingsErrorCode::CorruptFile);

    // The recent-project record lives in the same directory and must never be
    // accepted here, however similar its header looks.
    const auto project_file = temporary.file("recent-project.bin");
    RECENT_CHECK(persist::save_recent_project_path(
        project_file, temporary.file("show.mstage")));
    std::vector<std::byte> project_bytes;
    {
        std::ifstream input{project_file, std::ios::binary | std::ios::ate};
        RECENT_CHECK(static_cast<bool>(input));
        project_bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char *>(project_bytes.data()),
                   static_cast<std::streamsize>(project_bytes.size()));
    }
    rewrite(project_bytes);
    const auto wrong_record = persist::load_preferences(file);
    RECENT_CHECK(!wrong_record);
    RECENT_CHECK(wrong_record.error().code ==
                 persist::SettingsErrorCode::InvalidFormat);

    auto truncated = bytes;
    truncated.resize(bytes.size() - 1U);
    rewrite(truncated);
    RECENT_CHECK(!persist::load_preferences(file));
}

void preferences_sit_beside_the_recent_project_record() {
    const auto preferences = persist::default_preferences_file();
    const auto recent = persist::default_recent_project_settings_file();
    if (!preferences || !recent) {
        // A machine with no settings root is a supported, nonfatal state.
        return;
    }
    RECENT_CHECK(preferences.value().is_absolute());
    RECENT_CHECK(preferences.value() != recent.value());
    RECENT_CHECK(preferences.value().parent_path() ==
                 recent.value().parent_path());
    RECENT_CHECK(preferences.value().filename() == "preferences.bin");
}

void workspace_layout_bytes_round_trip_atomically() {
    const TempDirectory temporary;
    const auto file = temporary.file("nested/workspace-layout.bin");
    const auto missing = persist::load_workspace_layout(file);
    RECENT_CHECK(missing);
    RECENT_CHECK(missing.value().empty());

    const std::array payload{std::byte{0x00U}, std::byte{0xffU},
                             std::byte{0x17U}, std::byte{0x80U},
                             std::byte{0x42U}};
    RECENT_CHECK(persist::save_workspace_layout(file, payload));
    const auto installed = read_bytes(file);
    const auto loaded = persist::load_workspace_layout(file);
    RECENT_CHECK(loaded);
    RECENT_CHECK(std::equal(payload.begin(), payload.end(),
                            loaded.value().begin(), loaded.value().end()));

    const std::vector<std::byte> oversized(
        persist::max_workspace_layout_setting_bytes + 1U);
    const auto rejected = persist::save_workspace_layout(file, oversized);
    RECENT_CHECK(!rejected);
    RECENT_CHECK(rejected.error().code ==
                 persist::SettingsErrorCode::SizeLimitExceeded);
    RECENT_CHECK(read_bytes(file) == installed);
}

void damaged_workspace_layout_envelopes_are_rejected() {
    const TempDirectory temporary;
    const auto file = temporary.file("workspace-layout.bin");
    const std::array payload{std::byte{0x21U}, std::byte{0x43U}};
    RECENT_CHECK(persist::save_workspace_layout(file, payload));

    auto bytes = read_bytes(file);
    bytes.back() ^= std::byte{0x01U};
    write_bytes(file, bytes);
    const auto corrupt = persist::load_workspace_layout(file);
    RECENT_CHECK(!corrupt);
    RECENT_CHECK(corrupt.error().code ==
                 persist::SettingsErrorCode::CorruptFile);
}

void workspace_layout_sits_with_the_other_per_user_settings() {
    const auto layout = persist::default_workspace_layout_file();
    const auto preferences = persist::default_preferences_file();
    if (!layout || !preferences) {
        return;
    }
    RECENT_CHECK(layout.value().is_absolute());
    RECENT_CHECK(layout.value().parent_path() ==
                 preferences.value().parent_path());
    RECENT_CHECK(layout.value().filename() == "workspace-layout.bin");
}

void the_default_location_is_per_user_and_outside_the_project() {
    const auto located = persist::default_recent_project_settings_file();
    // Sandboxed/service accounts may intentionally expose no user profile.
    // That is the documented nonfatal startup fallback, not a test failure.
    if (!located) {
        RECENT_CHECK(located.error().code ==
                     persist::SettingsErrorCode::SettingsUnavailable);
        return;
    }
    RECENT_CHECK(located.has_value());
    RECENT_CHECK(located.value().is_absolute());
    RECENT_CHECK(located.value().filename() == "recent-project.bin");
    RECENT_CHECK(located.value().parent_path().filename() == "settings");
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 18U> cases{{
    {"missing settings are a normal startup",
     &missing_settings_are_an_empty_normal_startup},
    {"paths round trip and create the settings directory",
     &paths_round_trip_absolutely_and_settings_create_their_directory},
    {"replacement is atomic and invalid input preserves it",
     &replacing_a_setting_is_atomic_and_invalid_input_preserves_it},
    {"recent projects stay bounded, unique, and newest first",
     &recent_projects_are_bounded_unique_and_newest_first},
    {"corruption, truncation, and trailing data are rejected",
     &corruption_truncation_and_trailing_data_are_rejected},
    {"startup returns only a valid existing project",
     &startup_returns_only_a_fully_valid_existing_project},
    {"the default location is per-user",
     &the_default_location_is_per_user_and_outside_the_project},
    {"unconfigured preferences are empty, not an error",
     &unconfigured_preferences_are_empty_not_an_error},
    {"preferences round trip including non-ASCII paths",
     &preferences_round_trip_including_non_ascii_paths},
    {"an empty field means no preference and survives a round trip",
     &an_empty_field_means_no_preference_and_survives_a_round_trip},
    {"a saved preference replaces the previous one atomically",
     &a_saved_preference_replaces_the_previous_one_atomically},
    {"invalid playback tuning does not replace saved preferences",
     &invalid_playback_tuning_is_refused_without_replacing_settings},
    {"render machine settings round trip beside the editor settings",
     &render_machine_settings_round_trip_beside_the_editor_settings},
    {"damaged preferences are reported rather than silently defaulted",
     &damaged_preferences_are_reported_rather_than_silently_defaulted},
    {"preferences sit beside the recent-project record",
     &preferences_sit_beside_the_recent_project_record},
    {"workspace layout bytes round trip atomically",
     &workspace_layout_bytes_round_trip_atomically},
    {"damaged workspace layout envelopes are rejected",
     &damaged_workspace_layout_envelopes_are_rejected},
    {"workspace layout sits with the other per-user settings",
     &workspace_layout_sits_with_the_other_per_user_settings},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto &entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
