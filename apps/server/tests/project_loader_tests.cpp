#include "project_loader.hpp"

#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PL_CHECK(...)                                                          \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
  public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-loader-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] std::string file(std::string_view name) const {
        return app::native_path_to_utf8(path_ / std::string{name});
    }

  private:
    std::filesystem::path path_;
};

// The real file read, remembering what it was asked to read. Its answers can
// be held back, which is how a slow file server is stood in for.
class Reader final {
  public:
    void hold() {
        const std::lock_guard lock{mutex_};
        held_ = true;
    }
    void release() {
        {
            const std::lock_guard lock{mutex_};
            held_ = false;
        }
        changed_.notify_all();
    }
    // Blocks until a read is waiting on a held answer.
    void wait_until_asked() {
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this] { return waiting_; });
    }
    [[nodiscard]] std::vector<std::string> asked() {
        const std::lock_guard lock{mutex_};
        return asked_;
    }
    [[nodiscard]] app::ProjectReader function() {
        return [this](const std::string &path) {
            {
                std::unique_lock lock{mutex_};
                asked_.push_back(path);
                if (held_) {
                    waiting_ = true;
                    changed_.notify_all();
                    changed_.wait(lock, [this] { return !held_; });
                    waiting_ = false;
                }
            }
            return persist::load_project_file(app::native_path_from_utf8(path));
        };
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> asked_;
    bool held_{};
    bool waiting_{};
};

[[nodiscard]] doc::Node placement(std::uint64_t id, std::string source) {
    doc::Node node{};
    node.id = doc::NodeId{id};
    node.type = std::string{scene::schema::media_type};
    node.properties.insert_or_assign(std::string{scene::schema::source},
                                     doc::PropertyValue{std::move(source)});
    return node;
}

[[nodiscard]] std::optional<std::int64_t> whole(const doc::Root &root,
                                                std::uint64_t id,
                                                std::string_view property) {
    const doc::Node *const node = root.get(doc::NodeId{id});
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto found = node->properties.find(property);
    if (found == node->properties.end()) {
        return std::nullopt;
    }
    const auto *const value = std::get_if<std::int64_t>(&found->second);
    return value != nullptr ? std::optional<std::int64_t>{*value}
                            : std::nullopt;
}

void write(const std::string &path, const doc::Root &project) {
    const auto saved =
        persist::save_project_file(app::native_path_from_utf8(path), project);
    PL_CHECK(saved);
}

// The document comes back as the file has it: a clip that records no rate is
// not given one, and one that does keeps its own.
void a_project_is_read_as_the_file_has_it() {
    const TempDirectory temp;
    const std::string path = temp.file("opening night.mstage");
    doc::Node authored = placement(4'004U, "D:/media/sting.mov");
    authored.properties.insert_or_assign(
        std::string{scene::schema::source_rate_numerator},
        doc::PropertyValue{std::int64_t{50}});
    authored.properties.insert_or_assign(
        std::string{scene::schema::source_rate_denominator},
        doc::PropertyValue{std::int64_t{1}});
    write(path, doc::Root{}
                    .set(placement(4'001U, "D:/media/plate_%04u.png"))
                    .set(authored));

    Reader reader;
    app::ProjectLoader loader{reader.function()};
    const std::uint64_t ticket = loader.request(path, std::nullopt);
    PL_CHECK(ticket != 0U);
    loader.drain();

    const auto loaded = loader.take();
    PL_CHECK(loaded.has_value());
    PL_CHECK(loaded->ticket == ticket);
    PL_CHECK(loaded->path == path);
    PL_CHECK(loaded->project);
    const doc::Root &root = loaded->project.value();
    PL_CHECK(root.size() == 2U);
    PL_CHECK(!whole(root, 4'001U, scene::schema::source_rate_numerator));
    PL_CHECK(whole(root, 4'004U, scene::schema::source_rate_numerator) == 50);
    PL_CHECK(reader.asked() == std::vector<std::string>{path});
    // Taken once.
    PL_CHECK(!loader.take().has_value());
    PL_CHECK(loader.superseded() == 0U);
}

void a_document_already_decoded_is_not_read_from_disk() {
    const TempDirectory temp;
    // No such file: reading it would fail.
    const std::string path = temp.file("never written.mstage");
    Reader reader;
    app::ProjectLoader loader{reader.function()};
    const std::uint64_t ticket = loader.request(
        path, doc::Root{}.set(placement(7'310U, "D:/media/sky.mp4")));
    loader.drain();

    const auto loaded = loader.take();
    PL_CHECK(loaded.has_value());
    PL_CHECK(loaded->ticket == ticket);
    PL_CHECK(loaded->path == path);
    PL_CHECK(loaded->project);
    PL_CHECK(loaded->project.value().get(doc::NodeId{7'310U}) != nullptr);
    PL_CHECK(reader.asked().empty());
}

void a_file_that_cannot_be_read_is_answered_with_the_reason() {
    const TempDirectory temp;
    // The default reader: the real project-file load.
    app::ProjectLoader loader;
    const std::string path = temp.file("missing.mstage");
    const std::uint64_t ticket = loader.request(path, std::nullopt);
    loader.drain();

    const auto loaded = loader.take();
    PL_CHECK(loaded.has_value());
    PL_CHECK(loaded->ticket == ticket);
    PL_CHECK(loaded->path == path);
    PL_CHECK(!loaded->project);
    PL_CHECK(loaded->project.error().code ==
             persist::ProjectFileErrorCode::OpenFailed);
}

// The saved run's asset permit names control frames of a cohort that no
// longer exists; the loader hands back a document without it.
void the_saved_runs_asset_permit_is_left_out() {
    doc::Node stage{};
    stage.id = doc::NodeId{9'120U};
    stage.type = std::string{scene::schema::stage_type};
    stage.properties.insert_or_assign(
        std::string{scene::schema::asset_commit_digest},
        doc::PropertyValue{std::int64_t{771'203}});
    stage.properties.insert_or_assign(
        std::string{scene::schema::asset_commit_control},
        doc::PropertyValue{std::int64_t{5'512}});
    stage.properties.insert_or_assign(
        "kept", doc::PropertyValue{std::int64_t{-64}});
    app::ProjectLoader loader;
    static_cast<void>(loader.request({}, doc::Root{}.set(stage)));
    loader.drain();

    const auto loaded = loader.take();
    PL_CHECK(loaded.has_value() && loaded->project);
    const doc::Root &root = loaded->project.value();
    PL_CHECK(root.get(doc::NodeId{9'120U}) != nullptr);
    PL_CHECK(!whole(root, 9'120U, scene::schema::asset_commit_digest));
    PL_CHECK(!whole(root, 9'120U, scene::schema::asset_commit_control));
    PL_CHECK(whole(root, 9'120U, "kept") == -64);
}

// The reason the loader exists. While one project is still being read - held
// here, a slow share in life - asking for another returns at once, nothing
// half-read is handed out, and the project adopted is the last one asked for.
void a_request_never_waits_and_the_newest_project_wins() {
    const TempDirectory temp;
    const std::string slow = temp.file("slow show.mstage");
    const std::string skipped = temp.file("skipped show.mstage");
    const std::string wanted = temp.file("wanted show.mstage");
    write(slow, doc::Root{}.set(placement(1'001U, "D:/media/slow.mov")));
    write(skipped, doc::Root{}.set(placement(2'001U, "D:/media/skipped.mov")));
    write(wanted, doc::Root{}.set(placement(3'001U, "D:/media/wanted.mov")));

    Reader reader;
    reader.hold();
    app::ProjectLoader loader{reader.function()};
    const std::uint64_t first = loader.request(slow, std::nullopt);
    reader.wait_until_asked();

    // The worker is inside a read and stays there until release(); these
    // calls returning at all is the property.
    PL_CHECK(!loader.take().has_value());
    const std::uint64_t second = loader.request(skipped, std::nullopt);
    PL_CHECK(!loader.take().has_value());
    const std::uint64_t third = loader.request(wanted, std::nullopt);
    PL_CHECK(first != 0U && second > first && third > second);
    PL_CHECK(!loader.take().has_value());
    // One load under way overtaken, one waiting replaced.
    PL_CHECK(loader.superseded() == 2U);

    reader.release();
    loader.drain();
    const auto loaded = loader.take();
    PL_CHECK(loaded.has_value());
    PL_CHECK(loaded->ticket == third);
    PL_CHECK(loaded->path == wanted);
    PL_CHECK(loaded->project);
    PL_CHECK(loaded->project.value().get(doc::NodeId{3'001U}) != nullptr);
    PL_CHECK(!loader.take().has_value());
    PL_CHECK(loader.superseded() == 2U);

    // The replaced request was never read.
    PL_CHECK(reader.asked() == (std::vector<std::string>{slow, wanted}));
}

// A loader taken down while a read is still out returns as soon as that read
// does.
void a_loader_can_be_taken_down_mid_load() {
    const TempDirectory temp;
    const std::string path = temp.file("held show.mstage");
    write(path, doc::Root{}.set(placement(5'001U, "D:/media/plate.mov")));
    Reader reader;
    reader.hold();
    std::thread releasing;
    {
        app::ProjectLoader loader{reader.function()};
        static_cast<void>(loader.request(path, std::nullopt));
        reader.wait_until_asked();
        releasing = std::thread{[&] {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            reader.release();
        }};
        // The destructor waits here for the held read, then for nothing.
    }
    releasing.join();
    PL_CHECK(reader.asked().size() == 1U);
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array cases{
    Case{"a project is read as the file has it",
         &a_project_is_read_as_the_file_has_it},
    Case{"a document already decoded is not read from disk",
         &a_document_already_decoded_is_not_read_from_disk},
    Case{"a file that cannot be read is answered with the reason",
         &a_file_that_cannot_be_read_is_answered_with_the_reason},
    Case{"the saved run's asset permit is left out",
         &the_saved_runs_asset_permit_is_left_out},
    Case{"a request never waits and the newest project wins",
         &a_request_never_waits_and_the_newest_project_wins},
    Case{"a loader can be taken down mid-load",
         &a_loader_can_be_taken_down_mid_load},
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
