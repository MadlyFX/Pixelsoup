#pragma once

#include "lifecycle_agent.hpp"

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/persist/settings.hpp>

#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// A render machine with no command line.
//
// `pixelsoup` launched with no arguments - a double-click, a shortcut, a
// sign-in entry - runs this machine's lifecycle agent from settings kept in
// its per-user Preferences, and the agent starts the render process exactly
// as scripts/run-output-node.ps1 would. The first launch, with nothing
// stored, asks for those settings instead. They describe this machine, so
// they are never project data and never leave it.
//
// Everything here is plain logic over persist::Preferences, so the stored
// path - what a later launch starts from - is tested without a window, a
// network, or a second machine.
namespace mediaserver::app {

inline constexpr std::uint16_t render_machine_default_director_port = 45'060U;
// Node N's agent listens on this plus N, which is where the editor's Hardware
// Map looks for node N with the default project control port.
inline constexpr std::uint16_t render_machine_base_control_port = 47'840U;
// The show identity the bundled scripts and the editor demo use.
inline constexpr std::string_view render_machine_default_show_id =
    "0000000000000001000000000000000B";
inline constexpr std::size_t render_machine_max_password_bytes = 256U;

struct RenderMachineConfig final {
    asio::ip::tcp::endpoint director;
    bool discover_director{};
    bool automatic_index{};
    net::Id128 node_id{};
    core::u32 node_index{};
    std::uint16_t control_port{};
    bool automatic_control_port{};
    net::Id128 show_id{};
    // The key the agent requires: the one the stored render node password
    // derives, or the unsecured key when none is stored.
    std::string lifecycle_key;
    bool password_set{};

    bool operator==(const RenderMachineConfig&) const = default;
};

enum class RenderMachineConfigIssue : core::u8 {
    // Nothing stored: the first launch, which opens the setup form.
    NotSetUp,
    // Something stored that cannot be used. The form opens with the message.
    Invalid,
};

struct RenderMachineConfigError final {
    RenderMachineConfigIssue issue{RenderMachineConfigIssue::NotSetUp};
    std::string message;

    bool operator==(const RenderMachineConfigError&) const = default;
};

// What a launch with stored settings starts from.
[[nodiscard]] core::Result<RenderMachineConfig, RenderMachineConfigError>
render_machine_config(const persist::Preferences& stored);

// The default control port for a node index (base plus index).
[[nodiscard]] std::uint16_t
render_machine_default_control_port(core::u32 node_index) noexcept;

// The render process's identity, derived from its node index exactly as the
// scripts derive it, so node 0 keeps the identity the bundled demo uses.
[[nodiscard]] net::Id128 render_machine_node_id(core::u32 node_index) noexcept;

// The render process's own arguments - what run-output-node.ps1 passes with
// its defaults. The agent appends --node-index itself.
[[nodiscard]] std::vector<std::string>
render_machine_child_arguments(const RenderMachineConfig& config);

// The agent this machine runs: listening on every interface, supervising
// `executable` (this program) with the arguments above, and starting it at
// once.
[[nodiscard]] LifecycleAgentConfig
render_machine_agent_config(const RenderMachineConfig& config,
                            const std::filesystem::path& executable);

// The Director identity an editor opened from the render-machine window uses
// when Preferences hold none: the bundled scripts' show and epoch, listening
// on every interface so render machines elsewhere can reach it.
inline constexpr std::string_view render_machine_default_director_epoch =
    "0000000000000003000000000000000C";

// The arguments that open the operator editor as this machine's Director, as
// scripts/run-editor.ps1 would. A Director identity stored in Preferences is
// left to the editor to read, so only what is missing is passed here.
[[nodiscard]] std::vector<std::string>
editor_launch_arguments(const persist::Preferences& stored);

// What a render-machine launch starts from.
struct RenderMachineWindowOptions final {
    // This program, which the agent supervises as the render process.
    std::filesystem::path executable;
    // Empty when this machine has no per-user settings location: the form
    // still starts the machine, for this run only.
    std::filesystem::path preferences_file;
    persist::Preferences stored;
    // Set when the stored settings could not be read.
    std::string load_message;
    // This process has no console, so neither should the render process.
    bool no_console_window{};
};

// ------------------------------------------------------------- setup form

// The setup form as typed. Text throughout, so a half-typed value is kept
// while the operator corrects another one.
struct RenderMachineForm final {
    // Empty discovers the editor for this show. Otherwise a numeric address.
    // "ADDRESS:PORT" is accepted too,
    // and then wins over director_port.
    std::string director_address;
    std::string director_port;
    std::string node_index;
    // Empty is the default for the node index.
    std::string control_port;
    // Empty adopts the discovered editor's show.
    std::string show_id;
    // A password is stored and this form has not cleared it.
    bool password_set{};
    // Typed in this form. Never filled from storage, which keeps only the key.
    std::string password;
    // The operator typed or cleared a password; otherwise the stored key is
    // kept as it is.
    bool password_edited{};

    bool operator==(const RenderMachineForm&) const = default;
};

enum class RenderMachineField : core::u8 {
    DirectorAddress,
    DirectorPort,
    NodeIndex,
    ControlPort,
    ShowId,
    Password,
};

struct RenderMachineFormError final {
    RenderMachineField field{RenderMachineField::DirectorAddress};
    std::string message;

    bool operator==(const RenderMachineFormError&) const = default;
};

// The form as the stored settings fill it: defaults where nothing is stored.
[[nodiscard]] RenderMachineForm
render_machine_form(const persist::Preferences& stored);

// `stored` with its render-machine fields - and the render node password,
// when the form changed it - replaced by the form's. Every other field is
// left as it is, so the editor's settings on the same machine survive.
[[nodiscard]] core::Result<persist::Preferences, RenderMachineFormError>
apply_render_machine_form(const RenderMachineForm& form,
                          persist::Preferences stored);

// -------------------------------------------------------- Windows Firewall

struct FirewallRule final {
    std::string name;
    // "TCP" or "UDP".
    std::string protocol;
    std::uint16_t port{};

    bool operator==(const FirewallRule&) const = default;
};

// The inbound rules a render machine needs: its control port, and the
// discovery port when discovery is on. Named per port, so replacing one never
// removes another agent's.
[[nodiscard]] std::vector<FirewallRule>
render_machine_firewall_rules(std::uint16_t control_port,
                              std::uint16_t discovery_port);

// The arguments for `cmd.exe` that replace exactly those rules - each is
// deleted by name, then added - so pressing the button twice leaves one copy.
// Run elevated; nothing else on the machine is changed.
[[nodiscard]] std::string
firewall_rules_command(const std::vector<FirewallRule>& rules);

} // namespace mediaserver::app
