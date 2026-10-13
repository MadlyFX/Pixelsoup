#include "managed_process.hpp"

#include <chrono>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mediaserver::app {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] net::ClusterLifecycleReply reply(
    net::ClusterLifecycleOutcome outcome, net::ClusterProcessState state,
    core::u64 generation, std::string message) {
    return net::ClusterLifecycleReply{
        outcome, state, 0U, generation, std::move(message)};
}

#if defined(_WIN32)

[[nodiscard]] std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), output.data(), required);
    return converted == required ? output : std::wstring{};
}

// Windows command-line quoting as consumed by CommandLineToArgvW-compatible
// runtimes. CreateProcess receives the executable separately, so this string
// is data for the fixed child process and never passes through a shell.
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view value) {
    std::wstring output{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            output.append(slashes * 2U + 1U, L'\\');
            output.push_back(character);
            slashes = 0U;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0U;
        output.push_back(character);
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'\"');
    return output;
}

#endif

} // namespace

namespace detail {

class ManagedRenderProcessState final {
public:
    explicit ManagedRenderProcessState(ManagedRenderProcessConfig configured)
        : config(std::move(configured)) {}

    [[nodiscard]] net::ClusterLifecycleReply status() {
#if defined(_WIN32)
        if (process == nullptr) {
            return reply(net::ClusterLifecycleOutcome::NoChange, state,
                         generation, state == net::ClusterProcessState::Faulted
                                         ? "process faulted"
                                         : "process stopped");
        }
        const DWORD wait = WaitForSingleObject(process, 0U);
        if (wait == WAIT_TIMEOUT) {
            state = net::ClusterProcessState::Running;
            return reply(net::ClusterLifecycleOutcome::NoChange, state,
                         generation, "process running");
        }
        if (wait != WAIT_OBJECT_0) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process status failed");
        }
        DWORD exit_code{};
        if (GetExitCodeProcess(process, &exit_code) == 0) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process exit status failed");
        }
        CloseHandle(process);
        process = nullptr;
        if (job != nullptr) {
            CloseHandle(job);
            job = nullptr;
        }
        state = exit_code == 0U ? net::ClusterProcessState::Stopped
                                : net::ClusterProcessState::Faulted;
        return reply(net::ClusterLifecycleOutcome::NoChange, state,
                     generation,
                     exit_code == 0U ? "process exited"
                                     : "process exited with failure");
#else
        if (pid <= 0) {
            return reply(net::ClusterLifecycleOutcome::NoChange, state,
                         generation, state == net::ClusterProcessState::Faulted
                                         ? "process faulted"
                                         : "process stopped");
        }
        int process_status{};
        const pid_t waited = waitpid(pid, &process_status, WNOHANG);
        if (waited == 0) {
            state = net::ClusterProcessState::Running;
            return reply(net::ClusterLifecycleOutcome::NoChange, state,
                         generation, "process running");
        }
        if (waited < 0) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process status failed");
        }
        pid = -1;
        state = WIFEXITED(process_status) && WEXITSTATUS(process_status) == 0
                    ? net::ClusterProcessState::Stopped
                    : net::ClusterProcessState::Faulted;
        return reply(net::ClusterLifecycleOutcome::NoChange, state,
                     generation,
                     state == net::ClusterProcessState::Stopped
                         ? "process exited"
                         : "process exited with failure");
#endif
    }

    [[nodiscard]] net::ClusterLifecycleReply start() {
        const auto current = status();
        if (current.state == net::ClusterProcessState::Running) {
            return reply(net::ClusterLifecycleOutcome::NoChange,
                         current.state, generation, "already running");
        }
        state = net::ClusterProcessState::Starting;
#if defined(_WIN32)
        const std::wstring executable = config.executable.wstring();
        if (executable.empty()) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "invalid executable");
        }
        std::wstring command_line = quote_windows_argument(executable);
        for (const std::string& argument : config.arguments) {
            const std::wstring wide = utf8_to_wide(argument);
            if (wide.empty() && !argument.empty()) {
                state = net::ClusterProcessState::Faulted;
                return reply(net::ClusterLifecycleOutcome::Failed, state,
                             generation, "invalid UTF-8 argument");
            }
            command_line.push_back(L' ');
            command_line += quote_windows_argument(wide);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        HANDLE created_job = CreateJobObjectW(nullptr, nullptr);
        if (created_job == nullptr) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process job creation failed");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
        job_limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(
                created_job, JobObjectExtendedLimitInformation, &job_limits,
                sizeof(job_limits)) == 0) {
            CloseHandle(created_job);
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process job configuration failed");
        }
        PROCESS_INFORMATION created{};
        const std::wstring working = config.working_directory.empty()
                                         ? std::wstring{}
                                         : config.working_directory.wstring();
        const DWORD creation_flags =
            CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED |
            (config.no_console_window ? CREATE_NO_WINDOW : 0U);
        if (CreateProcessW(
                executable.c_str(), command_line.data(), nullptr, nullptr,
                FALSE, creation_flags, nullptr,
                working.empty() ? nullptr : working.c_str(), &startup,
                &created) == 0) {
            CloseHandle(created_job);
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process launch failed");
        }
        if (AssignProcessToJobObject(created_job, created.hProcess) == 0 ||
            ResumeThread(created.hThread) == static_cast<DWORD>(-1)) {
            static_cast<void>(TerminateProcess(created.hProcess, 1U));
            static_cast<void>(WaitForSingleObject(created.hProcess, 1'000U));
            CloseHandle(created.hThread);
            CloseHandle(created.hProcess);
            CloseHandle(created_job);
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process job assignment failed");
        }
        CloseHandle(created.hThread);
        process = created.hProcess;
        job = created_job;
#else
        if (!config.working_directory.empty()) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "working directory unsupported");
        }
        const std::string executable = config.executable.string();
        std::vector<std::string> owned;
        owned.reserve(config.arguments.size() + 1U);
        owned.push_back(executable);
        owned.insert(owned.end(), config.arguments.begin(),
                     config.arguments.end());
        std::vector<char*> argv;
        argv.reserve(owned.size() + 1U);
        for (std::string& argument : owned) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        posix_spawnattr_t attributes{};
        int error = posix_spawnattr_init(&attributes);
        const bool attributes_initialized = error == 0;
        if (error == 0) {
            error = posix_spawnattr_setflags(&attributes,
                                             POSIX_SPAWN_SETPGROUP);
        }
        if (error == 0) {
            error = posix_spawnattr_setpgroup(&attributes, 0);
        }
        pid_t created{};
        if (error == 0) {
            error = posix_spawn(&created, executable.c_str(), nullptr,
                                &attributes, argv.data(), environ);
        }
        if (attributes_initialized) {
            static_cast<void>(posix_spawnattr_destroy(&attributes));
        }
        if (error != 0) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process launch failed");
        }
        pid = created;
#endif
        state = net::ClusterProcessState::Running;
        ++generation;
        return reply(net::ClusterLifecycleOutcome::Applied, state, generation,
                     "process started");
    }

    [[nodiscard]] net::ClusterLifecycleReply stop() {
        const auto current = status();
        if (current.state == net::ClusterProcessState::Stopped) {
            return reply(net::ClusterLifecycleOutcome::NoChange,
                         current.state, generation, "already stopped");
        }
        if (current.state == net::ClusterProcessState::Faulted
#if defined(_WIN32)
            && process == nullptr
#else
            && pid <= 0
#endif
        ) {
            state = net::ClusterProcessState::Stopped;
            return reply(net::ClusterLifecycleOutcome::Applied, state,
                         generation, "fault cleared");
        }
        state = net::ClusterProcessState::Stopping;
#if defined(_WIN32)
        // This is deliberately a forced, bounded teardown. The Job Object
        // includes every descendant so an orphan helper cannot survive a
        // render-node relaunch or listener shutdown.
        if (job == nullptr || TerminateJobObject(job, 0U) == 0 ||
            WaitForSingleObject(process, static_cast<DWORD>(
                                      config.stop_timeout.count())) !=
                WAIT_OBJECT_0) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process stop failed");
        }
        CloseHandle(process);
        process = nullptr;
        CloseHandle(job);
        job = nullptr;
#else
        // The child owns a process group; signal the entire group so helper
        // processes cannot outlive a cluster stop/restart.
        if (kill(-pid, SIGTERM) != 0 && errno != ESRCH) {
            state = net::ClusterProcessState::Faulted;
            return reply(net::ClusterLifecycleOutcome::Failed, state,
                         generation, "process stop failed");
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              config.stop_timeout;
        int process_status{};
        while (std::chrono::steady_clock::now() < deadline &&
               waitpid(pid, &process_status, WNOHANG) == 0) {
            std::this_thread::sleep_for(10ms);
        }
        if (waitpid(pid, &process_status, WNOHANG) == 0) {
            static_cast<void>(kill(-pid, SIGKILL));
            static_cast<void>(waitpid(pid, &process_status, 0));
        }
        pid = -1;
#endif
        state = net::ClusterProcessState::Stopped;
        return reply(net::ClusterLifecycleOutcome::Applied, state, generation,
                     "process stopped");
    }

    ManagedRenderProcessConfig config;
    std::mutex mutex;
    core::u64 generation{};
    net::ClusterProcessState state{net::ClusterProcessState::Stopped};
#if defined(_WIN32)
    HANDLE process{};
    HANDLE job{};
#else
    pid_t pid{-1};
#endif
};

} // namespace detail

ManagedRenderProcess::ManagedRenderProcess(ManagedRenderProcessConfig config)
    : state_(std::make_unique<detail::ManagedRenderProcessState>(
          std::move(config))) {}

ManagedRenderProcess::~ManagedRenderProcess() {
    if (state_) {
        static_cast<void>(apply(net::ClusterLifecycleCommand::Stop));
    }
}

bool ManagedRenderProcess::valid_configuration(
    const ManagedRenderProcessConfig& config) noexcept {
    return !config.executable.empty() &&
#if !defined(_WIN32)
           config.working_directory.empty() &&
#endif
           config.stop_timeout > std::chrono::milliseconds::zero() &&
           config.stop_timeout <= std::chrono::minutes{1};
}

net::ClusterLifecycleReply ManagedRenderProcess::apply(
    net::ClusterLifecycleCommand command) noexcept {
    try {
        const std::lock_guard lock{state_->mutex};
        switch (command) {
        case net::ClusterLifecycleCommand::Status:
            return state_->status();
        case net::ClusterLifecycleCommand::Start:
            return state_->start();
        case net::ClusterLifecycleCommand::Restart: {
            const auto stopped = state_->stop();
            if (stopped.outcome == net::ClusterLifecycleOutcome::Failed) {
                return stopped;
            }
            return state_->start();
        }
        case net::ClusterLifecycleCommand::Stop:
            return state_->stop();
        }
    } catch (const std::bad_alloc&) {
        state_->state = net::ClusterProcessState::Faulted;
        return reply(net::ClusterLifecycleOutcome::Failed, state_->state,
                     state_->generation, "allocation failed");
    } catch (...) {
        state_->state = net::ClusterProcessState::Faulted;
        return reply(net::ClusterLifecycleOutcome::Failed, state_->state,
                     state_->generation, "process error");
    }
    return reply(net::ClusterLifecycleOutcome::Failed,
                 net::ClusterProcessState::Faulted, state_->generation,
                 "invalid command");
}

} // namespace mediaserver::app
