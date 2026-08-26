#include "../managed_process.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;
using namespace std::chrono_literals;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define PROCESS_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

std::filesystem::path test_executable;

void immutable_child_is_started_restarted_and_stopped() {
    app::ManagedRenderProcess process{app::ManagedRenderProcessConfig{
        test_executable, {"--managed-process-test-child"}, {}, 1s}};

    auto initial = process.apply(net::ClusterLifecycleCommand::Status);
    PROCESS_CHECK(initial.state == net::ClusterProcessState::Stopped);

    auto started = process.apply(net::ClusterLifecycleCommand::Start);
    PROCESS_CHECK(started.outcome == net::ClusterLifecycleOutcome::Applied);
    PROCESS_CHECK(started.state == net::ClusterProcessState::Running);
    PROCESS_CHECK(started.generation == 1U);

    auto duplicate = process.apply(net::ClusterLifecycleCommand::Start);
    PROCESS_CHECK(duplicate.outcome == net::ClusterLifecycleOutcome::NoChange);
    PROCESS_CHECK(duplicate.generation == 1U);

    auto restarted = process.apply(net::ClusterLifecycleCommand::Restart);
    PROCESS_CHECK(restarted.outcome == net::ClusterLifecycleOutcome::Applied);
    PROCESS_CHECK(restarted.state == net::ClusterProcessState::Running);
    PROCESS_CHECK(restarted.generation == 2U);

    auto stopped = process.apply(net::ClusterLifecycleCommand::Stop);
    PROCESS_CHECK(stopped.outcome == net::ClusterLifecycleOutcome::Applied);
    PROCESS_CHECK(stopped.state == net::ClusterProcessState::Stopped);
    PROCESS_CHECK(stopped.generation == 2U);
}

void missing_executable_fails_honestly() {
    app::ManagedRenderProcess process{app::ManagedRenderProcessConfig{
        test_executable.parent_path() / "definitely-missing-render-node.exe",
        {}, {}, 250ms}};
    auto started = process.apply(net::ClusterLifecycleCommand::Start);
    PROCESS_CHECK(started.outcome == net::ClusterLifecycleOutcome::Failed);
    PROCESS_CHECK(started.state == net::ClusterProcessState::Faulted);
    PROCESS_CHECK(started.generation == 0U);
}

void windows_still_active_exit_code_is_not_misread_as_running() {
    app::ManagedRenderProcess process{app::ManagedRenderProcessConfig{
        test_executable, {"--managed-process-exit-259"}, {}, 1s}};
    auto started = process.apply(net::ClusterLifecycleCommand::Start);
    PROCESS_CHECK(started.state == net::ClusterProcessState::Running);
    net::ClusterLifecycleReply status{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        std::this_thread::sleep_for(5ms);
        status = process.apply(net::ClusterLifecycleCommand::Status);
    } while (status.state == net::ClusterProcessState::Running &&
             std::chrono::steady_clock::now() < deadline);
    PROCESS_CHECK(status.state == net::ClusterProcessState::Faulted);
    PROCESS_CHECK(status.generation == 1U);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string_view{argv[1]} == "--managed-process-test-child") {
        for (;;) {
            std::this_thread::sleep_for(1s);
        }
    }
    if (argc == 2 &&
        std::string_view{argv[1]} == "--managed-process-exit-259") {
        return 259;
    }
    test_executable = std::filesystem::absolute(argv[0]);
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"immutable child lifecycle",
         &immutable_child_is_started_restarted_and_stopped},
        {"missing executable reports failure",
         &missing_executable_fails_honestly},
        {"exit code 259 is not still active",
         &windows_still_active_exit_code_is_not_misread_as_running},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
