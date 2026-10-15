#pragma once

#include "render_machine.hpp"

#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The few things a render machine started without a command line needs from
// the operating system: leaving the console a double-click opened, letting
// the control machine in through Windows Firewall, and coming back after a
// restart. Everything here is a no-op answer ("not supported") off Windows.
namespace mediaserver::app {

// Launched from Explorer, a shortcut or the sign-in list, this console
// program is given a console window nobody reads. When this process is the
// only one on its console, leaves it - the window closes - and returns true.
// Run from a terminal, the console is shared with the shell and is kept, so
// the log stays where the person who typed the command can see it. Also true
// when there was no console to begin with.
[[nodiscard]] bool leave_unshared_console() noexcept;

// This program's own path, for the agent to supervise and the sign-in entry
// to start. Empty when the system cannot say.
[[nodiscard]] std::filesystem::path current_executable();

// A message box, for the one failure a process without a console could
// otherwise only report to nobody: its window not opening. Off Windows it
// writes to standard error.
void show_error_dialog(std::string_view title, std::string_view text);

// Starts `executable` with `arguments` as a process of its own, which keeps
// running after this one exits. `no_console_window` as for the render
// process. Empty when it started; otherwise why not.
[[nodiscard]] std::string
start_independent_process(const std::filesystem::path& executable,
                          const std::vector<std::string>& arguments,
                          const std::filesystem::path& working_directory,
                          bool no_console_window);

enum class FirewallStatus : core::u8 {
    // Not Windows, or the firewall could not be asked.
    Unknown,
    // Off for every network this machine is on.
    Off,
    // Every rule render_machine_firewall_rules names is present and enabled.
    RulesPresent,
    RulesMissing,
};

[[nodiscard]] FirewallStatus
query_firewall(const std::vector<FirewallRule>& rules);

// `cmd.exe` running elevated - Windows asks the operator first. Polled from
// the window loop so the window keeps drawing while it runs.
class ElevatedCommand final {
public:
    struct Impl;

    // Starts `parameters` (see firewall_rules_command). Empty with `error`
    // set when it could not start, including the operator saying no.
    [[nodiscard]] static std::unique_ptr<ElevatedCommand>
    start(const std::string& parameters, std::string& error);

    ElevatedCommand(const ElevatedCommand&) = delete;
    ElevatedCommand& operator=(const ElevatedCommand&) = delete;
    ~ElevatedCommand();

    // Its exit code once it has finished.
    [[nodiscard]] std::optional<unsigned long> poll() noexcept;

private:
    explicit ElevatedCommand(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Starting this render machine when the operator signs in to Windows: an
// entry in the per-user Run list naming `executable` --render-machine.
[[nodiscard]] bool start_at_sign_in_supported() noexcept;
[[nodiscard]] bool starts_at_sign_in(const std::filesystem::path& executable);
[[nodiscard]] bool set_start_at_sign_in(const std::filesystem::path& executable,
                                        bool enabled);
// The command the Run entry holds.
[[nodiscard]] std::string
sign_in_command(const std::filesystem::path& executable);

} // namespace mediaserver::app
