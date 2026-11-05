#include "render_machine.hpp"

#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/persist/settings.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define RM_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto timestamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("mediaserver-render-machine-" + std::to_string(timestamp) +
                 "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / std::filesystem::path{name};
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] bool contains(const std::vector<std::string>& values,
                            std::string_view wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

[[nodiscard]] bool any_starts_with(const std::vector<std::string>& values,
                                   std::string_view prefix) {
    return std::any_of(values.begin(), values.end(),
                       [prefix](const std::string& value) {
                           return std::string_view{value}.starts_with(prefix);
                       });
}

// What an operator types on a fresh machine.
[[nodiscard]] app::RenderMachineForm typed_form() {
    app::RenderMachineForm form =
        app::render_machine_form(persist::Preferences{});
    form.director_address = "192.168.1.20";
    form.node_index = "3";
    form.show_id = std::string{app::render_machine_default_show_id};
    return form;
}

void a_first_launch_has_nothing_stored_and_opens_the_form() {
    const auto config = app::render_machine_config(persist::Preferences{});
    RM_CHECK(!config);
    RM_CHECK(config.error().issue == app::RenderMachineConfigIssue::NotSetUp);

    // The form starts from the values a typical render machine keeps: the
    // Director's usual port, node 0, the automatic control port, no password.
    const auto form = app::render_machine_form(persist::Preferences{});
    RM_CHECK(form.director_address.empty());
    RM_CHECK(form.director_port == "45060");
    RM_CHECK(form.node_index.empty());
    RM_CHECK(form.control_port.empty());
    RM_CHECK(form.show_id.empty());
    RM_CHECK(!form.password_set);
    RM_CHECK(!form.password_edited);
}

void the_form_is_saved_once_and_later_launches_start_from_it() {
    const TempDirectory temporary;
    const auto file = temporary.file("preferences.bin");

    // First launch: the form, filled in once and saved.
    const auto applied =
        app::apply_render_machine_form(typed_form(), persist::Preferences{});
    RM_CHECK(applied);
    RM_CHECK(persist::save_preferences(file, applied.value()));

    // A later launch reads the file and goes straight to the agent.
    const auto stored = persist::load_preferences(file);
    RM_CHECK(stored);
    RM_CHECK(stored.value().render_machine_director == "192.168.1.20:45060");
    const auto config = app::render_machine_config(stored.value());
    RM_CHECK(config);
    RM_CHECK(config.value().director.address().to_string() == "192.168.1.20");
    RM_CHECK(config.value().director.port() == 45'060U);
    RM_CHECK(config.value().node_index == 3U);
    // Where the editor's Hardware Map looks for node 3.
    RM_CHECK(config.value().control_port == 47'843U);
    RM_CHECK(!config.value().password_set);
    RM_CHECK(net::lifecycle_key_is_unsecured(config.value().lifecycle_key));

    // The agent this machine runs: every interface, this program as the
    // render process, started at once, and found by discovery.
    const std::filesystem::path executable =
        temporary.file("media-server.exe");
    const auto agent = app::render_machine_agent_config(config.value(),
                                                        executable);
    RM_CHECK(agent.listen.address().is_unspecified());
    RM_CHECK(agent.listen.address().is_v4());
    RM_CHECK(agent.listen.port() == 47'843U);
    RM_CHECK(agent.node_index == 3U);
    RM_CHECK(agent.child == executable);
    RM_CHECK(agent.autostart);
    RM_CHECK(agent.discovery_port == net::cluster_discovery_port);
    RM_CHECK(agent.secret == config.value().lifecycle_key);

    // The render process gets what run-output-node.ps1 would give it.
    const auto& arguments = agent.child_arguments;
    RM_CHECK(contains(arguments, "--role=render,output"));
    RM_CHECK(contains(arguments, "--director-connect=192.168.1.20:45060"));
    RM_CHECK(contains(arguments,
                      "--show-id=0000000000000001000000000000000B"));
    RM_CHECK(any_starts_with(arguments, "--node-id="));
    RM_CHECK(config.value().node_id);
    RM_CHECK(stored.value().render_machine_node_id.size() == 32U);
    RM_CHECK(contains(arguments,
                      "--node-incarnation=00000000000000000000000000000001"));
    RM_CHECK(contains(arguments, "--playback"));
    // The Feed Map is part of the output path; --scene would bypass it.
    RM_CHECK(contains(arguments, "--output"));
    RM_CHECK(!contains(arguments, "--scene"));
    RM_CHECK(contains(arguments, "--borderless"));
    // The agent appends its own --node-index.
    RM_CHECK(!any_starts_with(arguments, "--node-index="));
}

void node_identities_follow_the_scripts() {
    // Node 0 keeps the identity the bundled demo uses.
    const auto node_zero = app::render_machine_node_id(0U);
    RM_CHECK(node_zero.high == 0U);
    RM_CHECK(node_zero.low == 0x2AU);
    RM_CHECK(app::render_machine_node_id(5U).low == 0x2FU);
    RM_CHECK(app::render_machine_default_control_port(0U) == 47'840U);
    RM_CHECK(app::render_machine_default_control_port(12U) == 47'852U);
}

void the_password_is_kept_only_as_its_key_and_survives_an_untouched_form() {
    auto form = typed_form();
    form.password = "hunter2";
    form.password_edited = true;
    const auto with_password =
        app::apply_render_machine_form(form, persist::Preferences{});
    RM_CHECK(with_password);
    const std::string expected = net::lifecycle_key_from_password("hunter2");
    RM_CHECK(with_password.value().lifecycle_password_key == expected);
    const auto config = app::render_machine_config(with_password.value());
    RM_CHECK(config);
    RM_CHECK(config.value().password_set);
    RM_CHECK(config.value().lifecycle_key == expected);

    // Reopened, the form says a password is set but never shows it, and
    // saving it without touching the field keeps the key.
    auto reopened = app::render_machine_form(with_password.value());
    RM_CHECK(reopened.password_set);
    RM_CHECK(reopened.password.empty());
    reopened.node_index = "4";
    const auto untouched =
        app::apply_render_machine_form(reopened, with_password.value());
    RM_CHECK(untouched);
    RM_CHECK(untouched.value().lifecycle_password_key == expected);
    RM_CHECK(untouched.value().render_machine_node_index == 4U);

    // Clearing it is a choice that sticks: the agent runs unsecured.
    reopened.password.clear();
    reopened.password_edited = true;
    reopened.password_set = false;
    const auto cleared =
        app::apply_render_machine_form(reopened, untouched.value());
    RM_CHECK(cleared);
    RM_CHECK(cleared.value().lifecycle_password_key.empty());
    const auto open = app::render_machine_config(cleared.value());
    RM_CHECK(open);
    RM_CHECK(!open.value().password_set);

    // One the editor's Preferences saved on this machine is the same key the
    // agent requires: the render node password is one setting.
    persist::Preferences from_editor = cleared.value();
    from_editor.lifecycle_password_key =
        net::lifecycle_key_from_password("stage-left");
    const auto shared = app::render_machine_config(from_editor);
    RM_CHECK(shared);
    RM_CHECK(shared.value().lifecycle_key == from_editor.lifecycle_password_key);

    // A damaged key never reads as "no password".
    from_editor.lifecycle_password_key = "not a key";
    const auto damaged = app::render_machine_config(from_editor);
    RM_CHECK(!damaged);
    RM_CHECK(damaged.error().issue == app::RenderMachineConfigIssue::Invalid);

    auto too_long = typed_form();
    too_long.password = std::string(257U, 'p');
    too_long.password_edited = true;
    const auto refused =
        app::apply_render_machine_form(too_long, persist::Preferences{});
    RM_CHECK(!refused);
    RM_CHECK(refused.error().field == app::RenderMachineField::Password);
}

void saving_the_form_keeps_the_editors_own_settings() {
    persist::Preferences stored{};
    stored.director_listen = "0.0.0.0:45060";
    stored.show_id = "0000000000000001000000000000000B";
    stored.director_epoch = "0000000000000003000000000000000C";
    stored.wal_path = "D:\\shows\\editor.wal";
    stored.output_buffer_frames = 30U;
    const auto applied = app::apply_render_machine_form(typed_form(), stored);
    RM_CHECK(applied);
    RM_CHECK(applied.value().director_listen == stored.director_listen);
    RM_CHECK(applied.value().show_id == stored.show_id);
    RM_CHECK(applied.value().director_epoch == stored.director_epoch);
    RM_CHECK(applied.value().wal_path == stored.wal_path);
    RM_CHECK(applied.value().output_buffer_frames == 30U);
    RM_CHECK(applied.value().render_machine_node_index == 3U);
}

void advanced_values_are_optional_and_round_trip() {
    auto form = typed_form();
    form.control_port = "47950";
    form.show_id = "0000000000000001000000000000000c";
    const auto applied =
        app::apply_render_machine_form(form, persist::Preferences{});
    RM_CHECK(applied);
    RM_CHECK(applied.value().render_machine_control_port == 47'950U);
    // Normalized, so the same show never looks like two.
    RM_CHECK(applied.value().render_machine_show_id ==
             "0000000000000001000000000000000C");
    const auto config = app::render_machine_config(applied.value());
    RM_CHECK(config);
    RM_CHECK(config.value().control_port == 47'950U);
    RM_CHECK(contains(app::render_machine_child_arguments(config.value()),
                      "--show-id=0000000000000001000000000000000C"));

    const auto reopened = app::render_machine_form(applied.value());
    RM_CHECK(reopened.director_address == "192.168.1.20");
    RM_CHECK(reopened.director_port == "45060");
    RM_CHECK(reopened.node_index == "3");
    RM_CHECK(reopened.control_port == "47950");
    RM_CHECK(reopened.show_id == "0000000000000001000000000000000C");
}

void addresses_are_numeric_and_may_carry_their_port() {
    const auto director_of = [](std::string address, std::string port) {
        auto form = typed_form();
        form.director_address = std::move(address);
        form.director_port = std::move(port);
        return app::apply_render_machine_form(form, persist::Preferences{});
    };
    // Pasted with its port, the address wins over the port field.
    const auto pasted = director_of("10.0.0.5:45100", "45060");
    RM_CHECK(pasted);
    RM_CHECK(pasted.value().render_machine_director == "10.0.0.5:45100");
    // Spaces around a pasted address are not the operator's mistake.
    const auto padded = director_of("  10.0.0.6 ", " 45061 ");
    RM_CHECK(padded);
    RM_CHECK(padded.value().render_machine_director == "10.0.0.6:45061");
    // An empty port is the usual one.
    const auto usual = director_of("10.0.0.7", "");
    RM_CHECK(usual);
    RM_CHECK(usual.value().render_machine_director == "10.0.0.7:45060");
    const auto bracketed = director_of("[fd00::5]:45060", "");
    RM_CHECK(bracketed);
    RM_CHECK(bracketed.value().render_machine_director == "[fd00::5]:45060");
    const auto bare_v6 = director_of("fd00::6", "45060");
    RM_CHECK(bare_v6);
    RM_CHECK(bare_v6.value().render_machine_director == "[fd00::6]:45060");
    const auto v6_config = app::render_machine_config(bare_v6.value());
    RM_CHECK(v6_config);
    RM_CHECK(contains(app::render_machine_child_arguments(v6_config.value()),
                      "--director-connect=[fd00::6]:45060"));

    for (const std::string_view refused :
         {"control-room", "render01.local", "0.0.0.0", "::",
          "224.0.0.1", "255.255.255.255", "10.0.0", "10.0.0.5:99999"}) {
        const auto result = director_of(std::string{refused}, "45060");
        RM_CHECK(!result);
        RM_CHECK(result.error().field ==
                     app::RenderMachineField::DirectorAddress ||
                 result.error().field == app::RenderMachineField::DirectorPort);
        RM_CHECK(!result.error().message.empty());
    }
    // A name is told apart from a typo, since the fix is different.
    const auto named = director_of("control-room", "45060");
    RM_CHECK(!named);
    RM_CHECK(named.error().message.find("not its name") != std::string::npos);

    for (const std::string_view port : {"0", "65536", "port", "-1"}) {
        const auto result = director_of("10.0.0.5", std::string{port});
        RM_CHECK(!result);
        RM_CHECK(result.error().field == app::RenderMachineField::DirectorPort);
    }
}

void out_of_range_numbers_and_identities_are_named_in_the_form() {
    const auto refused = [](auto edit) {
        auto form = typed_form();
        edit(form);
        return app::apply_render_machine_form(form, persist::Preferences{});
    };
    for (const std::string_view index : {"4096", "-1", "two", "1.5"}) {
        const auto result = refused([index](app::RenderMachineForm& form) {
            form.node_index = std::string{index};
        });
        RM_CHECK(!result);
        RM_CHECK(result.error().field == app::RenderMachineField::NodeIndex);
    }
    const auto highest = refused(
        [](app::RenderMachineForm& form) { form.node_index = "4095"; });
    RM_CHECK(highest);
    for (const std::string_view port : {"0", "65536", "auto"}) {
        const auto result = refused([port](app::RenderMachineForm& form) {
            form.control_port = std::string{port};
        });
        RM_CHECK(!result);
        RM_CHECK(result.error().field == app::RenderMachineField::ControlPort);
    }
    for (const std::string_view show :
         {"1", "00000000000000000000000000000000",
          "0000000000000001000000000000000G",
          "0000000000000001000000000000000B0"}) {
        const auto result = refused([show](app::RenderMachineForm& form) {
            form.show_id = std::string{show};
        });
        RM_CHECK(!result);
        RM_CHECK(result.error().field == app::RenderMachineField::ShowId);
    }
}

void stored_settings_that_no_longer_work_reopen_the_form() {
    persist::Preferences stored{};
    stored.render_machine_director = "control-room:45060";
    const auto config = app::render_machine_config(stored);
    RM_CHECK(!config);
    RM_CHECK(config.error().issue == app::RenderMachineConfigIssue::Invalid);
    RM_CHECK(config.error().message.find("control-room:45060") !=
             std::string::npos);
    // The form shows what was stored, so the operator sees what to fix.
    const auto form = app::render_machine_form(stored);
    RM_CHECK(form.director_address == "control-room:45060");

    stored.render_machine_director = "10.0.0.5:45060";
    stored.render_machine_show_id = "not-a-show";
    const auto show = app::render_machine_config(stored);
    RM_CHECK(!show);
    RM_CHECK(show.error().issue == app::RenderMachineConfigIssue::Invalid);
}

void firewall_rules_name_their_ports_and_replace_themselves() {
    const auto rules = app::render_machine_firewall_rules(47'843U, 47'839U);
    RM_CHECK(rules.size() == 2U);
    RM_CHECK(rules[0].protocol == "TCP");
    RM_CHECK(rules[0].port == 47'843U);
    RM_CHECK(rules[0].name.find("47843") != std::string::npos);
    RM_CHECK(rules[1].protocol == "UDP");
    RM_CHECK(rules[1].port == 47'839U);
    RM_CHECK(rules[0].name != rules[1].name);
    // Without discovery there is nothing to let in on UDP.
    RM_CHECK(app::render_machine_firewall_rules(47'840U, 0U).size() == 1U);

    const std::string command = app::firewall_rules_command(rules);
    RM_CHECK(command.starts_with("/c netsh "));
    for (const auto& rule : rules) {
        const std::string quoted = "name=\"" + rule.name + "\"";
        // Deleted first, then added: pressing the button twice leaves one.
        const auto deleted =
            command.find("delete rule " + quoted);
        const auto added = command.find("add rule " + quoted);
        RM_CHECK(deleted != std::string::npos);
        RM_CHECK(added != std::string::npos);
        RM_CHECK(deleted < added);
        RM_CHECK(command.find("protocol=" + rule.protocol + " localport=" +
                              std::to_string(rule.port)) != std::string::npos);
    }
    RM_CHECK(command.find("dir=in action=allow") != std::string::npos);
    // The second add runs only when the first succeeded, so the exit status
    // reports the first failure.
    RM_CHECK(command.find(" && netsh advfirewall firewall add rule") !=
             std::string::npos);
    RM_CHECK(command.find("action=block") == std::string::npos);
}

struct Case final {
    const char* name;
    void (*function)();
};

void the_editor_opens_as_a_director_on_stored_or_default_identity() {
    // Nothing stored: the editor needs a full Director identity to listen.
    const auto fresh = app::editor_launch_arguments(persist::Preferences{});
    RM_CHECK(contains(fresh, "--role=director"));
    RM_CHECK(contains(fresh, "--playback"));
    RM_CHECK(contains(fresh, "--edit"));
    RM_CHECK(contains(fresh, "--director-listen=0.0.0.0:45060"));
    RM_CHECK(contains(fresh, "--show-id=0000000000000001000000000000000B"));
    RM_CHECK(contains(fresh, "--director-epoch=0000000000000003000000000000000C"));
    RM_CHECK(fresh.size() == 6U);

    // A stored identity is the editor's to read: passing it would also mark
    // it as overridden by the command line in Preferences.
    persist::Preferences stored{};
    stored.director_listen = "10.1.2.3:45101";
    stored.show_id = "000000000000000A000000000000000D";
    stored.director_epoch = "000000000000000E000000000000000F";
    const auto configured = app::editor_launch_arguments(stored);
    RM_CHECK(configured.size() == 3U);
    for (const std::string& argument : configured) {
        RM_CHECK(!argument.starts_with("--director-listen=") &&
                 !argument.starts_with("--show-id=") &&
                 !argument.starts_with("--director-epoch="));
    }
}

void automatic_discovery_is_saved_without_a_stale_address() {
    const TempDirectory temporary;
    auto form = app::render_machine_form(persist::Preferences{});
    form.node_index = "7";
    form.show_id = "00000000000000110000000000000023";
    const auto applied = app::apply_render_machine_form(form, persist::Preferences{});
    RM_CHECK(applied);
    RM_CHECK(applied.value().render_machine_director == "auto");
    RM_CHECK(persist::save_preferences(temporary.file("auto.bin"), applied.value()));
    const auto loaded = persist::load_preferences(temporary.file("auto.bin"));
    RM_CHECK(loaded);
    const auto config = app::render_machine_config(loaded.value());
    RM_CHECK(config);
    RM_CHECK(config.value().discover_director);
    RM_CHECK(config.value().node_index == 7U);
    RM_CHECK(config.value().show_id.high == 0x11U);
    RM_CHECK(config.value().show_id.low == 0x23U);
    RM_CHECK(contains(app::render_machine_child_arguments(config.value()),
                      "--director-connect=auto"));
    RM_CHECK(app::render_machine_form(loaded.value()).director_address.empty());
    // Clearing an old manual override enables discovery without storing the
    // currently discovered IP, so a later DHCP change is resolved on restart.
    auto manual = typed_form();
    const auto old = app::apply_render_machine_form(manual, loaded.value());
    RM_CHECK(old);
    manual = app::render_machine_form(old.value());
    manual.director_address = "   ";
    const auto cleared = app::apply_render_machine_form(manual, old.value());
    RM_CHECK(cleared);
    RM_CHECK(cleared.value().render_machine_director == "auto");
    const auto fresh = app::apply_render_machine_form(
        app::render_machine_form(persist::Preferences{}), persist::Preferences{});
    RM_CHECK(fresh);
    RM_CHECK(fresh.value().render_machine_node_index == net::automatic_node_index);
    RM_CHECK(fresh.value().render_machine_show_id.empty());
    RM_CHECK(fresh.value().render_machine_node_id.size() == 32U);
    RM_CHECK(persist::save_preferences(temporary.file("full-auto.bin"), fresh.value()));
    const auto reloaded = persist::load_preferences(temporary.file("full-auto.bin"));
    RM_CHECK(reloaded);
    const auto identity = app::render_machine_config(reloaded.value());
    RM_CHECK(identity && identity.value().automatic_index && !identity.value().show_id);
    const auto resaved = app::apply_render_machine_form(
        app::render_machine_form(reloaded.value()), reloaded.value());
    RM_CHECK(resaved);
    RM_CHECK(resaved.value().render_machine_node_id == fresh.value().render_machine_node_id);
    persist::Preferences existing_machine;
    existing_machine.render_machine_director = "192.0.2.5:45123";
    existing_machine.render_machine_node_index = 3U;
    const auto switched = app::apply_render_machine_form(
        app::render_machine_form(persist::Preferences{}), existing_machine);
    RM_CHECK(switched);
    RM_CHECK(switched.value().render_machine_node_id == "0000000000000000000000000000002D");
}

constexpr std::array<Case, 12U> cases{{
    {"automatic discovery is saved without a stale address",
     &automatic_discovery_is_saved_without_a_stale_address},
    {"a first launch has nothing stored and opens the form",
     &a_first_launch_has_nothing_stored_and_opens_the_form},
    {"the form is saved once and later launches start from it",
     &the_form_is_saved_once_and_later_launches_start_from_it},
    {"node identities follow the scripts",
     &node_identities_follow_the_scripts},
    {"the password is kept only as its key and survives an untouched form",
     &the_password_is_kept_only_as_its_key_and_survives_an_untouched_form},
    {"saving the form keeps the editor's own settings",
     &saving_the_form_keeps_the_editors_own_settings},
    {"advanced values are optional and round trip",
     &advanced_values_are_optional_and_round_trip},
    {"addresses are numeric and may carry their port",
     &addresses_are_numeric_and_may_carry_their_port},
    {"out-of-range numbers and identities are named in the form",
     &out_of_range_numbers_and_identities_are_named_in_the_form},
    {"stored settings that no longer work reopen the form",
     &stored_settings_that_no_longer_work_reopen_the_form},
    {"firewall rules name their ports and replace themselves",
     &firewall_rules_name_their_ports_and_replace_themselves},
    {"the editor opens as a director on stored or default identity",
     &the_editor_opens_as_a_director_on_stored_or_default_identity},
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
