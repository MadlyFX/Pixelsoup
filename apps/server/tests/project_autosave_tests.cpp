#include "project_autosave.hpp"

#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace app = mediaserver::app;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace scene = mediaserver::scene;
namespace show = mediaserver::show;

using Clock = app::ProjectAutosave::Clock;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PA_CHECK(...)                                                          \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
  public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-autosave-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    // The recovery folder itself does not exist until something is written.
    [[nodiscard]] std::filesystem::path recovery() const {
        return path_ / "recovery";
    }

  private:
    std::filesystem::path path_;
};

constexpr std::string_view show_file = "D:/shows/opening night.mstage";
constexpr doc::NodeId clip_id{4'217U};
constexpr doc::NodeId stage_id{9'031U};

[[nodiscard]] doc::Node clip(std::string label, double position) {
    doc::Node node{};
    node.id = clip_id;
    node.type = "test.clip";
    node.properties.insert_or_assign("label",
                                     doc::PropertyValue{std::move(label)});
    node.properties.insert_or_assign("position_x",
                                     doc::PropertyValue{position});
    node.properties.insert_or_assign("first_frame",
                                     doc::PropertyValue{std::int64_t{-37}});
    node.properties.insert_or_assign("locked", doc::PropertyValue{true});
    return node;
}

[[nodiscard]] doc::Node transport(std::int64_t marker) {
    doc::Node node{};
    node.id = show::transport_node_id;
    node.type = std::string{show::transport_node_type};
    node.properties.insert_or_assign("test_marker",
                                     doc::PropertyValue{marker});
    return node;
}

[[nodiscard]] doc::Root project(std::string label, double position) {
    return doc::Root{}.set(clip(std::move(label), position));
}

[[nodiscard]] bool present(const std::filesystem::path &file) {
    std::error_code error;
    return std::filesystem::exists(file, error) && !error;
}

void recovery_files_are_one_per_project() {
    const std::filesystem::path directory{"recovery-root"};
    const auto first = app::project_recovery_file(directory, show_file);
    PA_CHECK(first.parent_path() == directory);
    PA_CHECK(first.extension() == ".mstage");
    PA_CHECK(first == app::project_recovery_file(directory, show_file));
    PA_CHECK(first != app::project_recovery_file(
                          directory, "D:/shows/closing night.mstage"));
    // The same file however its path was spelled.
    PA_CHECK(first == app::project_recovery_file(
                          directory, "D:/shows/extra/../opening night.mstage"));
#if defined(_WIN32)
    PA_CHECK(first == app::project_recovery_file(
                          directory, "D:\\shows\\opening night.mstage"));
#endif
    const auto untitled = app::project_recovery_file(directory, {});
    PA_CHECK(untitled.parent_path() == directory);
    PA_CHECK(untitled != first);
}

void a_changed_project_is_written_and_reads_back() {
    const TempDirectory temp;
    app::ProjectAutosave autosave{temp.recovery()};
    const doc::Root changed = project("stormy sky", 12.625);
    const Clock::time_point now = Clock::now();

    PA_CHECK(autosave.due(now, changed.version(), show_file));
    autosave.observe(now, true, changed, transport(58), show_file);
    autosave.drain();

    PA_CHECK(autosave.counters() ==
             app::ProjectAutosaveCounters{1U, 0U, 0U, 0U});
    const auto loaded = persist::load_project_file(
        app::project_recovery_file(temp.recovery(), show_file));
    PA_CHECK(loaded);
    const doc::Node *const read = loaded.value().get(clip_id);
    PA_CHECK(read != nullptr);
    PA_CHECK(*read == clip("stormy sky", 12.625));
    const doc::Node *const read_transport =
        loaded.value().get(show::transport_node_id);
    PA_CHECK(read_transport != nullptr);
    PA_CHECK(*read_transport == transport(58));
    // Nothing was written for any other project.
    PA_CHECK(!present(app::project_recovery_file(temp.recovery(), {})));
}

void the_copy_holds_what_save_would_write() {
    doc::Node stage{};
    stage.id = stage_id;
    stage.type = std::string{scene::schema::stage_type};
    stage.properties.insert_or_assign(
        std::string{scene::schema::asset_commit_digest},
        doc::PropertyValue{std::int64_t{771}});
    stage.properties.insert_or_assign(
        std::string{scene::schema::asset_commit_control},
        doc::PropertyValue{std::int64_t{4'400}});
    stage.properties.insert_or_assign(
        std::string{scene::schema::asset_commit_show},
        doc::PropertyValue{std::int64_t{912}});
    stage.properties.insert_or_assign("kept", doc::PropertyValue{2.5});
    const doc::Root live = project("plate", 3.0).set(stage);

    const doc::Root saved = app::saved_project_content(live, transport(11));

    const doc::Node *const written = saved.get(stage_id);
    PA_CHECK(written != nullptr);
    PA_CHECK(written->properties.size() == 1U);
    PA_CHECK(written->properties.contains("kept"));
    PA_CHECK(*saved.get(clip_id) == clip("plate", 3.0));
    PA_CHECK(*saved.get(show::transport_node_id) == transport(11));
    // The live document keeps its permit.
    PA_CHECK(live.get(stage_id)->properties.size() == 4U);
}

void the_project_is_looked_at_once_per_interval_and_change() {
    const TempDirectory temp;
    const std::chrono::seconds interval{7};
    app::ProjectAutosave autosave{temp.recovery(), interval};
    const doc::Root first = project("first", 1.0);
    const doc::Root second = project("second", 2.0).set(transport(1));
    PA_CHECK(first.version() != second.version());
    const Clock::time_point start = Clock::now();

    PA_CHECK(autosave.due(start, first.version(), show_file));
    autosave.observe(start, true, first, transport(1), show_file);
    // A newer document inside the interval waits for it.
    PA_CHECK(!autosave.due(start + std::chrono::seconds{6}, second.version(),
                           show_file));
    // The interval alone is not a reason: nothing changed.
    PA_CHECK(!autosave.due(start + interval, first.version(), show_file));
    PA_CHECK(autosave.due(start + interval, second.version(), show_file));
    // The same document under another name is another project.
    PA_CHECK(autosave.due(start + interval, first.version(),
                          "D:/shows/renamed.mstage"));
    // A loaded project may reuse the version the last one had.
    autosave.invalidate();
    PA_CHECK(!autosave.due(start + std::chrono::seconds{6}, first.version(),
                           show_file));
    PA_CHECK(autosave.due(start + interval, first.version(), show_file));
    autosave.drain();
}

void the_newest_snapshot_is_the_one_kept() {
    const TempDirectory temp;
    app::ProjectAutosave autosave{temp.recovery(), Clock::duration::zero()};
    const Clock::time_point now = Clock::now();
    constexpr std::uint64_t snapshots = 5U;
    for (std::uint64_t index = 0U; index < snapshots; ++index) {
        autosave.observe(now, true,
                         project("take " + std::to_string(index),
                                 static_cast<double>(index) + 0.25),
                         transport(static_cast<std::int64_t>(index)),
                         show_file);
    }
    autosave.drain();

    // Each snapshot was either written or replaced by a newer one, and the
    // last one asked for is what the file holds.
    const app::ProjectAutosaveCounters counters = autosave.counters();
    PA_CHECK(counters.written + counters.superseded == snapshots);
    PA_CHECK(counters.written >= 1U);
    PA_CHECK(counters.failed == 0U);
    const auto loaded = persist::load_project_file(
        app::project_recovery_file(temp.recovery(), show_file));
    PA_CHECK(loaded);
    PA_CHECK(*loaded.value().get(clip_id) == clip("take 4", 4.25));
    PA_CHECK(*loaded.value().get(show::transport_node_id) == transport(4));
}

void returning_to_the_saved_project_removes_the_copy() {
    const TempDirectory temp;
    const auto file = app::project_recovery_file(temp.recovery(), show_file);
    app::ProjectAutosave autosave{temp.recovery(), Clock::duration::zero()};
    const Clock::time_point now = Clock::now();

    autosave.observe(now, true, project("edited", 8.0), transport(2),
                     show_file);
    autosave.drain();
    PA_CHECK(present(file));

    // Undone back to what the file holds.
    autosave.observe(now, false, project("saved", 1.0), transport(2),
                     show_file);
    autosave.drain();
    PA_CHECK(!present(file));
    PA_CHECK(autosave.counters().removed == 1U);
}

void a_clean_project_leaves_an_earlier_runs_copy_alone() {
    const TempDirectory temp;
    const auto file = app::project_recovery_file(temp.recovery(), show_file);
    {
        app::ProjectAutosave earlier{temp.recovery()};
        earlier.observe(Clock::now(), true, project("lost work", 6.5),
                        transport(3), show_file);
    }
    PA_CHECK(present(file));

    // The next run opens the saved project, which is clean, while the offer
    // to recover is still unanswered.
    app::ProjectAutosave autosave{temp.recovery(), Clock::duration::zero()};
    autosave.observe(Clock::now(), false, project("saved", 1.0), transport(3),
                     show_file);
    autosave.drain();
    PA_CHECK(present(file));
    PA_CHECK(autosave.counters() ==
             app::ProjectAutosaveCounters{0U, 0U, 0U, 0U});
}

void saving_discards_the_copy_and_anything_queued() {
    const TempDirectory temp;
    const auto file = app::project_recovery_file(temp.recovery(), show_file);
    const auto other =
        app::project_recovery_file(temp.recovery(), "D:/shows/other.mstage");
    app::ProjectAutosave autosave{temp.recovery(), Clock::duration::zero()};
    const Clock::time_point now = Clock::now();

    autosave.observe(now, true, project("kept", 2.0), transport(4),
                     "D:/shows/other.mstage");
    autosave.observe(now, true, project("edited", 8.0), transport(4),
                     show_file);
    autosave.drain();
    PA_CHECK(present(file));
    PA_CHECK(present(other));

    // A snapshot still queued behind the save must not put the copy back.
    autosave.observe(now, true, project("edited again", 9.0), transport(4),
                     show_file);
    autosave.discard(show_file);
    autosave.drain();
    PA_CHECK(!present(file));
    // Another project's copy is not this one's to remove.
    PA_CHECK(present(other));
}

void a_removal_asked_for_at_shutdown_still_happens() {
    const TempDirectory temp;
    const auto file = app::project_recovery_file(temp.recovery(), {});
    {
        app::ProjectAutosave autosave{temp.recovery()};
        autosave.observe(Clock::now(), true, project("untitled work", 5.0),
                         transport(5), {});
        autosave.drain();
        PA_CHECK(present(file));
        autosave.discard({});
    }
    PA_CHECK(!present(file));
}

void write_copy(const std::filesystem::path &directory,
                std::string_view project_path, const doc::Root &content) {
    app::ProjectAutosave autosave{directory};
    autosave.observe(Clock::now(), true, content, transport(6), project_path);
}

void nothing_is_offered_without_a_copy() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    PA_CHECK(!app::find_project_recovery(temp.recovery(), show_file, &saved)
                  .has_value());
    PA_CHECK(
        !app::find_project_recovery(temp.recovery(), {}, nullptr).has_value());
}

void unsaved_changes_to_the_opened_project_are_offered() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    write_copy(temp.recovery(), show_file, project("unsaved", 44.5));

    const auto offered =
        app::find_project_recovery(temp.recovery(), show_file, &saved);

    PA_CHECK(offered.has_value());
    PA_CHECK(offered->project_path == show_file);
    PA_CHECK(*offered->project.get(clip_id) == clip("unsaved", 44.5));
    // "YYYY-MM-DD HH:MM" in local time.
    PA_CHECK(offered->saved_at.size() == 16U);
    PA_CHECK(offered->saved_at[4] == '-' && offered->saved_at[13] == ':');
    // Another project's copy is not offered for this one.
    PA_CHECK(!app::find_project_recovery(temp.recovery(),
                                         "D:/shows/other.mstage", &saved)
                  .has_value());
}

void a_copy_the_file_already_holds_is_not_offered() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    // Only the transport differs, which is where the playhead was.
    write_copy(temp.recovery(), show_file, saved);

    PA_CHECK(!app::find_project_recovery(temp.recovery(), show_file, &saved)
                  .has_value());
}

void untitled_work_is_offered_whatever_project_opens() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    write_copy(temp.recovery(), {}, project("never saved", 7.75));

    const auto opening =
        app::find_project_recovery(temp.recovery(), show_file, &saved);
    PA_CHECK(opening.has_value());
    PA_CHECK(opening->project_path.empty());
    PA_CHECK(*opening->project.get(clip_id) == clip("never saved", 7.75));

    const auto fresh = app::find_project_recovery(temp.recovery(), {}, nullptr);
    PA_CHECK(fresh.has_value());
    PA_CHECK(fresh->project_path.empty());
}

void the_newer_of_two_copies_is_offered() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    write_copy(temp.recovery(), show_file, project("in the show", 20.0));
    write_copy(temp.recovery(), {}, project("untitled", 30.0));
    const auto show_copy =
        app::project_recovery_file(temp.recovery(), show_file);
    const auto untitled_copy = app::project_recovery_file(temp.recovery(), {});
    const auto now = std::filesystem::file_time_type::clock::now();

    std::filesystem::last_write_time(show_copy, now - std::chrono::hours{3});
    std::filesystem::last_write_time(untitled_copy,
                                     now - std::chrono::hours{1});
    auto offered =
        app::find_project_recovery(temp.recovery(), show_file, &saved);
    PA_CHECK(offered.has_value());
    PA_CHECK(offered->project_path.empty());
    PA_CHECK(*offered->project.get(clip_id) == clip("untitled", 30.0));

    std::filesystem::last_write_time(show_copy, now - std::chrono::minutes{5});
    offered = app::find_project_recovery(temp.recovery(), show_file, &saved);
    PA_CHECK(offered.has_value());
    PA_CHECK(offered->project_path == show_file);
    PA_CHECK(*offered->project.get(clip_id) == clip("in the show", 20.0));
}

void a_damaged_copy_is_ignored() {
    const TempDirectory temp;
    const doc::Root saved = project("saved", 1.0);
    std::filesystem::create_directories(temp.recovery());
    {
        std::ofstream damaged{
            app::project_recovery_file(temp.recovery(), show_file),
            std::ios::binary};
        damaged << "this is not a project file";
    }
    PA_CHECK(!app::find_project_recovery(temp.recovery(), show_file, &saved)
                  .has_value());

    // A readable copy beside it is still found.
    write_copy(temp.recovery(), {}, project("untitled", 30.0));
    const auto offered =
        app::find_project_recovery(temp.recovery(), show_file, &saved);
    PA_CHECK(offered.has_value());
    PA_CHECK(offered->project_path.empty());
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array cases{
    Case{"recovery files are one per project",
         &recovery_files_are_one_per_project},
    Case{"a changed project is written and reads back",
         &a_changed_project_is_written_and_reads_back},
    Case{"the copy holds what Save would write",
         &the_copy_holds_what_save_would_write},
    Case{"the project is looked at once per interval and change",
         &the_project_is_looked_at_once_per_interval_and_change},
    Case{"the newest snapshot is the one kept",
         &the_newest_snapshot_is_the_one_kept},
    Case{"returning to the saved project removes the copy",
         &returning_to_the_saved_project_removes_the_copy},
    Case{"a clean project leaves an earlier run's copy alone",
         &a_clean_project_leaves_an_earlier_runs_copy_alone},
    Case{"saving discards the copy and anything queued",
         &saving_discards_the_copy_and_anything_queued},
    Case{"a removal asked for at shutdown still happens",
         &a_removal_asked_for_at_shutdown_still_happens},
    Case{"nothing is offered without a copy",
         &nothing_is_offered_without_a_copy},
    Case{"unsaved changes to the opened project are offered",
         &unsaved_changes_to_the_opened_project_are_offered},
    Case{"a copy the file already holds is not offered",
         &a_copy_the_file_already_holds_is_not_offered},
    Case{"untitled work is offered whatever project opens",
         &untitled_work_is_offered_whatever_project_opens},
    Case{"the newer of two copies is offered",
         &the_newer_of_two_copies_is_offered},
    Case{"a damaged copy is ignored", &a_damaged_copy_is_ignored},
};

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
