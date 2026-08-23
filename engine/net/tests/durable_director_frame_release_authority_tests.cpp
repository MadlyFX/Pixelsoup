#include <mediaserver/net/durable_director_frame_release_authority.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace command = mediaserver::cmd;
namespace document = mediaserver::doc;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define AUTHORITY_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempStore final {
public:
    TempStore() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        const auto stem = std::string{"mediaserver-release-adapter-"}
            + std::to_string(timestamp) + "-"
            + std::to_string(sequence.fetch_add(1U));
        wal_ = std::filesystem::temp_directory_path() / (stem + ".wal");
        ledger_ = std::filesystem::temp_directory_path() / (stem + ".rel");
    }

    ~TempStore() {
        std::error_code ignored;
        std::filesystem::remove(wal_, ignored);
        std::filesystem::remove(ledger_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& wal() const noexcept {
        return wal_;
    }
    [[nodiscard]] const std::filesystem::path& ledger() const noexcept {
        return ledger_;
    }

private:
    std::filesystem::path wal_;
    std::filesystem::path ledger_;
};

[[nodiscard]] command::StreamCursor genesis_cursor() noexcept {
    return command::StreamCursor{
        command::FrameNumber{0U},
        document::Version{0U},
        command::CommandId{1U},
    };
}

[[nodiscard]] persist::CommandLog open_log(
    const std::filesystem::path& path) {
    persist::OpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::CommandLog::open_or_create(
        path, genesis_cursor(), options);
    AUTHORITY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] persist::ReleaseLedger open_ledger(
    const std::filesystem::path& path,
    const persist::CommandLog& log) {
    auto snapshot = log.capture_committed();
    AUTHORITY_CHECK(snapshot);
    persist::ReleaseLedgerOpenOptions options;
    options.sync_policy = persist::SyncPolicy::Data;
    auto opened = persist::ReleaseLedger::open_or_create(
        path, snapshot.value(), options);
    AUTHORITY_CHECK(opened);
    return std::move(opened).value();
}

[[nodiscard]] document::Node make_node(std::uint64_t frame) {
    return document::Node{
        document::NodeId{frame},
        "release.adapter.node",
        {{"frame", static_cast<std::int64_t>(frame)}},
    };
}

void schedule_put(
    command::DirectorScheduler& director,
    std::uint64_t frame) {
    auto scheduled = director.schedule_edit(
        command::PutNode{make_node(frame)},
        command::EraseNode{document::NodeId{frame}},
        command::UndoContextId{1U},
        command::TransactionId{frame},
        command::FrameNumber{frame});
    AUTHORITY_CHECK(scheduled);
}

void exact_publication_is_the_only_true_release() {
    TempStore store;
    auto log = open_log(store.wal());
    auto ledger = open_ledger(store.ledger(), log);
    command::DirectorScheduler director;
    persist::DurableDirector durable{director, log, ledger};
    net::DirectorSessionHub hub{durable};
    net::DurableDirectorFrameReleaseAuthority authority{durable};
    AUTHORITY_CHECK(authority.next_release_ordinal() == 1U);

    schedule_put(director, 1U);
    auto first = hub.seal_and_publish(command::FrameNumber{1U});
    AUTHORITY_CHECK(first);
    auto second = hub.seal_and_publish(command::FrameNumber{2U});
    AUTHORITY_CHECK(second);

    TempStore foreign_store;
    auto foreign_log = open_log(foreign_store.wal());
    command::DirectorScheduler foreign_director;
    schedule_put(foreign_director, 1U);
    persist::DurableDirector foreign_durable{foreign_director, foreign_log};
    net::DirectorSessionHub foreign_hub{foreign_durable};
    auto foreign = foreign_hub.seal_and_publish(command::FrameNumber{1U});
    AUTHORITY_CHECK(foreign);

    const auto initial_size = std::filesystem::file_size(store.ledger());
    AUTHORITY_CHECK(!authority.persist_release(foreign.value().frame, 1U));
    AUTHORITY_CHECK(
        std::filesystem::file_size(store.ledger()) == initial_size);
    AUTHORITY_CHECK(!authority.persist_release(second.value().frame, 1U));
    AUTHORITY_CHECK(
        std::filesystem::file_size(store.ledger()) == initial_size);
    AUTHORITY_CHECK(!authority.persist_release(first.value().frame, 2U));
    AUTHORITY_CHECK(
        std::filesystem::file_size(store.ledger()) == initial_size);
    AUTHORITY_CHECK(durable.state() == persist::DurableDirectorState::Ready);

    AUTHORITY_CHECK(authority.persist_release(first.value().frame, 1U));
    AUTHORITY_CHECK(authority.next_release_ordinal() == 2U);
    AUTHORITY_CHECK(
        authority.release_ordinal(
            first.value().frame.before_cursor(),
            first.value().frame.after_cursor())
        == 1U);
    auto wrong_after = first.value().frame.after_cursor();
    wrong_after.revision = document::Version{99U};
    AUTHORITY_CHECK(!authority.release_ordinal(
        first.value().frame.before_cursor(), wrong_after));

    AUTHORITY_CHECK(authority.persist_release(second.value().frame, 2U));
    AUTHORITY_CHECK(authority.next_release_ordinal() == 3U);
    AUTHORITY_CHECK(
        authority.release_ordinal(
            second.value().frame.before_cursor(),
            second.value().frame.after_cursor())
        == 2U);
    AUTHORITY_CHECK(durable.capture_resume_source());

    foreign_hub.close();
    hub.close();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"only exact publication emits true release",
         &exact_publication_is_the_only_true_release},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
