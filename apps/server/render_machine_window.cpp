#include "render_machine_window.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace mediaserver::app {

// ------------------------------------------------------------- the window

namespace {

[[nodiscard]] gui::FontAtlas load_font(float scale) {
    constexpr std::array<const char*, 6U> candidates{
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/local/share/fonts/DejaVuSans.ttf"};
    for (const char* path : candidates) {
        auto atlas = gui::FontAtlas::create(path, 14U, scale);
        if (atlas.has_value()) {
            return std::move(atlas).value();
        }
    }
    return gui::FontAtlas::empty();
}

[[nodiscard]] bool input_active(const gui::InputState& input) noexcept {
    for (const gui::MouseButton button :
         {gui::MouseButton::Left, gui::MouseButton::Middle,
          gui::MouseButton::Right}) {
        if (input.mouse_down(button) || input.mouse_pressed(button) ||
            input.mouse_released(button)) {
            return true;
        }
    }
    return input.mouse_dx() != 0.0F || input.mouse_dy() != 0.0F ||
           input.wheel() != 0.0F || !input.text().empty() ||
           !input.keys().empty();
}

[[nodiscard]] std::string
describe_start_failure(const LifecycleAgentStartError& error,
                       std::uint16_t control_port) {
    if (error.failure == LifecycleAgentFailure::Autostart) {
        return "The render process did not start: " + error.message + ".";
    }
    const std::string port = "TCP " + std::to_string(control_port);
    if (error.listener.system_error == std::errc::address_in_use ||
        error.listener.system_error ==
            std::error_code{10048, std::system_category()}) {
        return "Control port " + port +
               " is already in use - most likely another render machine or "
               "a script-started node on this computer. Close it, or choose "
               "another control port under Change Settings.";
    }
    std::string text = "Could not open control port " + port;
    if (error.listener.system_error) {
        text += ": " + error.listener.system_error.message();
    }
    return text + ".";
}

[[nodiscard]] std::string endpoint_text(const asio::ip::tcp::endpoint& endpoint) {
    std::error_code error;
    const std::string address = endpoint.address().to_string(error);
    if (error) {
        return "unavailable";
    }
    if (endpoint.address().is_v6()) {
        return '[' + address + "]:" + std::to_string(endpoint.port());
    }
    return address + ':' + std::to_string(endpoint.port());
}

} // namespace

int run_render_machine_window(RenderMachineWindowOptions options) {
    gui::WindowConfig window_config{};
    window_config.title = "Pixelsoup - Render Machine";
    window_config.width = render_machine_window_width;
    window_config.height = render_machine_window_height;
    window_config.logical_size = true;
    auto created = gui::UiWindow::create(window_config);
    if (!created) {
        std::cerr << "could not open the render machine window (code "
                  << static_cast<unsigned>(created.error())
                  << "); run the lifecycle agent from the command line "
                     "instead (see --help)\n";
        return 1;
    }
    gui::UiWindow window = std::move(created).value();
    gui::FontAtlas font = load_font(window.ui_scale());
    if (!window.set_atlas(font)) {
        std::cerr << "could not upload the render machine window's font\n";
        return 1;
    }
    gui::InputState input;
    gui::UiDrawList list;
    gui::UiContext ctx{input, list, font};

    RenderMachinePanel panel;
    RenderMachineStatus status{};
    status.hostname = net::cluster_discovery_hostname();
    // Both are Windows features, present together or not at all.
    status.firewall_supported = start_at_sign_in_supported();
    status.sign_in_supported = start_at_sign_in_supported();
    status.starts_at_sign_in = starts_at_sign_in(options.executable);

    std::unique_ptr<LifecycleAgent> agent;
    net::ClusterDiscoveryBrowserConfig editor_search_config;
    editor_search_config.kind = net::ClusterDiscoveryKind::Director;
    net::ClusterDiscoveryBrowser editor_browser{editor_search_config};
    const auto editor_search_error = editor_browser.start();
    std::optional<RenderMachineConfig> config;
    std::optional<RenderMachineConfig> pending_identity;
    std::unique_ptr<net::ClusterDiscoveryBrowser> enrollment_browser;
    core::u64 enrolled_instance{};
    std::vector<FirewallRule> rules;
    std::unique_ptr<ElevatedCommand> firewall_change;

    using clock = std::chrono::steady_clock;
    clock::time_point next_status{};
    clock::time_point next_slow{};
    clock::time_point choose_identity_after{};

    const auto refresh_slow = [&] {
        status.addresses.clear();
        for (const net::LocalIpv4Interface& local :
             net::local_ipv4_interfaces()) {
            std::error_code error;
            std::string text = local.address.to_string(error);
            if (!error) {
                status.addresses.push_back(std::move(text));
            }
        }
        if (status.firewall_supported && !rules.empty()) {
            status.firewall = query_firewall(rules);
        }
        next_slow = clock::now() + std::chrono::seconds{10};
    };

    const auto start_agent = [&](const RenderMachineConfig& wanted) {
        // The old agent goes first: it holds the port and the render process.
        agent.reset();
        config = wanted;
        status.agent_error.clear();
        if (wanted.automatic_index || !wanted.show_id) {
            pending_identity = wanted;
            enrollment_browser.reset();
            status.waiting_for_identity = true;
            status.agent_running = false;
            status.director = "Automatic discovery";
            rules.clear();
            next_status = {};
            choose_identity_after = clock::now() + editor_search_config.query_interval;
            return;
        }
        pending_identity.reset();
        enrollment_browser.reset();
        status.waiting_for_identity = false;
        status.process_message.clear();
        status.node_index = wanted.node_index;
        status.show_id = options.stored.render_machine_show_id.empty()
            ? "From discovered editor" : options.stored.render_machine_show_id;
        status.director = wanted.discover_director ? "Automatic discovery"
                                                   : endpoint_text(wanted.director);
        status.control_port = wanted.control_port;
        status.password_set = wanted.password_set;
        LifecycleAgentConfig agent_config =
            render_machine_agent_config(wanted, options.executable);
        agent_config.no_console_window = options.no_console_window;
        rules = render_machine_firewall_rules(wanted.control_port,
                                              agent_config.discovery_port);
        if (!LifecycleAgent::valid_configuration(agent_config)) {
            status.agent_error =
                "These settings do not make a valid render machine.";
        } else {
            try {
                auto candidate =
                    std::make_unique<LifecycleAgent>(std::move(agent_config));
                auto started = candidate->start();
                if (started) {
                    agent = std::move(candidate);
                    status.discovery = std::string{agent->discovery_text()};
                } else {
                    status.agent_error = describe_start_failure(
                        started.error(), wanted.control_port);
                }
            } catch (const std::bad_alloc&) {
                status.agent_error = "Out of memory starting the agent.";
            } catch (const std::system_error& error) {
                status.agent_error =
                    std::string{"Could not start the agent: "} + error.what();
            }
        }
        status.agent_running = agent != nullptr;
        next_status = {};
        refresh_slow();
    };

    // A launch with settings starts at once; the form is for the first run
    // and for settings that can no longer be used.
    if (!options.load_message.empty()) {
        panel.open_setup(render_machine_form(persist::Preferences{}),
                         options.load_message, false);
    } else if (auto stored = render_machine_config(options.stored)) {
        start_agent(stored.value());
        panel.show_status();
    } else {
        panel.open_setup(render_machine_form(options.stored),
                         stored.error().issue ==
                                 RenderMachineConfigIssue::Invalid
                             ? stored.error().message
                             : std::string{},
                         false);
    }

    bool open = true;
    while (open) {
        if (!window.pump(input)) {
            if (agent && !panel.confirming_close()) {
                panel.request_close();
                window.cancel_close_request();
            } else if (agent) {
                // A second close while asking is the answer.
                break;
            } else {
                break;
            }
        }
        if (const float scale = window.ui_scale();
            std::fabs(scale - font.scale()) > 0.001F) {
            font = load_font(scale);
            list.set_white_texel(font.white_u(), font.white_v());
            static_cast<void>(window.set_atlas(font));
        }
        list.set_scale(window.ui_scale());

        const auto now = clock::now();
        if (now >= next_status) {
            // Interface enumeration and network I/O stay on the browser worker.
            persist::Preferences search_preferences;
            search_preferences.render_machine_director = "auto";
            search_preferences.render_machine_show_id = panel.setup_open()
                ? panel.form().show_id : options.stored.render_machine_show_id;
            const auto search_identity = render_machine_config(search_preferences);
            const auto found = editor_browser.snapshot();
            status.discovered_editor.clear();
            if (const auto error = editor_search_error ? editor_search_error : found.error) {
                status.editor_search = "Editor discovery unavailable: " + error.message() +
                                       ". Enter a manual address.";
            } else if (!search_identity) {
                status.editor_search = "Enter a valid show ID to discover its editor.";
            } else if (const auto editor = net::discovered_director_identity(
                           found, search_identity.value().show_id,
                           pending_identity && !pending_identity->discover_director
                               ? std::optional<asio::ip::tcp::endpoint>{pending_identity->director}
                               : std::nullopt)) {
                status.discovered_editor = endpoint_text({editor->address, editor->control_port});
                status.editor_search = "Editor found: " + status.discovered_editor +
                                       ". Discovery is used on each process start.";
                if (pending_identity && now >= choose_identity_after) {
                    if (!enrollment_browser || enrolled_instance != editor->instance) {
                        auto request = editor_search_config;
                        request.node_id = pending_identity->node_id;
                        request.director_instance = editor->instance;
                        request.requested_node_index = pending_identity->automatic_index
                            ? net::automatic_node_index : pending_identity->node_index;
                        enrollment_browser = std::make_unique<net::ClusterDiscoveryBrowser>(request);
                        if (const auto enrollment_error = enrollment_browser->start()) {
                            status.agent_error = "Could not request a node index: " + enrollment_error.message();
                            status.waiting_for_identity = false;
                        }
                        enrolled_instance = editor->instance;
                    }
                    const auto assigned = net::discovered_director_identity(
                        enrollment_browser->snapshot(), editor->show_id);
                    if (assigned && assigned->assignment == net::NodeAssignmentStatus::Assigned) {
                        auto resolved = *pending_identity;
                        resolved.show_id = assigned->show_id;
                        resolved.node_index = assigned->node_index;
                        resolved.automatic_index = false;
                        if (resolved.automatic_control_port) {
                            resolved.control_port = render_machine_default_control_port(resolved.node_index);
                        }
                        start_agent(resolved);
                    } else if (assigned) {
                        status.waiting_for_identity = false;
                        status.agent_error = assigned->assignment == net::NodeAssignmentStatus::Conflict
                            ? "That node index belongs to another machine. Clear the index override to assign one automatically."
                            : "The editor could not save a node index. Check its node-assignment record and available slots.";
                    }
                }
            } else {
                const auto matches = std::count_if(found.machines.begin(), found.machines.end(),
                    [&](const net::ClusterDiscoveredMachine& machine) {
                        return !search_identity.value().show_id ||
                               machine.show_id == search_identity.value().show_id;
                    });
                status.editor_search = matches > 1
                    ? "Several editors found for this show. Close the extra editors or enter a manual address."
                    : "Waiting for an editor for this show. Open the editor and allow UDP 47839 and its show TCP port through its firewall.";
            }
            if (agent) {
                const auto reply =
                    agent->apply(net::ClusterLifecycleCommand::Status);
                status.process = reply.state;
                if (reply.state != net::ClusterProcessState::Running) {
                    status.process_message = reply.message;
                }
                const auto counters = agent->counters();
                status.accepted_requests = counters.authenticated_requests;
                status.refused_requests = counters.authentication_failures;
            }
            if (firewall_change) {
                if (const auto code = firewall_change->poll()) {
                    status.firewall_message =
                        *code == 0U
                            ? "Rules added."
                            : "Windows reported an error (" +
                                  std::to_string(*code) +
                                  ") - the rules may not have been added.";
                    firewall_change.reset();
                    next_slow = {};
                }
            }
            status.firewall_busy = firewall_change != nullptr;
            next_status = now + std::chrono::milliseconds{250};
        }
        if (now >= next_slow) {
            refresh_slow();
        }

        ctx.new_frame(window.time_seconds());
        const RenderMachineAction action = panel.draw(
            ctx,
            gui::Rect{0.0F, 0.0F, static_cast<float>(window.width()),
                      static_cast<float>(window.height())},
            status);
        ctx.draw_overlays(gui::Rect{0.0F, 0.0F,
                                    static_cast<float>(window.width()),
                                    static_cast<float>(window.height())});
        window.set_cursor(ctx.cursor());
        static_cast<void>(window.present(list, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));

        switch (action) {
        case RenderMachineAction::None:
            break;
        case RenderMachineAction::SaveAndStart: {
            // Onto the record as it is now: the editor may have saved its
            // own settings since this window opened.
            persist::Preferences base = options.stored;
            if (!options.preferences_file.empty()) {
                if (auto current =
                        persist::load_preferences(options.preferences_file)) {
                    base = std::move(current).value();
                }
            }
            auto applied = apply_render_machine_form(panel.form(), base);
            if (!applied) {
                panel.reject_form(applied.error());
                break;
            }
            auto wanted = render_machine_config(applied.value());
            if (!wanted) {
                panel.reject_form(RenderMachineFormError{
                    RenderMachineField::DirectorAddress,
                    wanted.error().message});
                break;
            }
            std::string saved_note;
            if (options.preferences_file.empty()) {
                saved_note = "This machine has no place to keep settings, so "
                             "these apply until the window is closed.";
            } else if (auto saved = persist::save_preferences(
                           options.preferences_file, applied.value());
                       !saved) {
                saved_note = "The settings could not be saved (code " +
                             std::to_string(static_cast<unsigned>(
                                 saved.error().code)) +
                             "), so they apply until the window is closed.";
            }
            options.stored = std::move(applied).value();
            // An unchanged machine keeps running rather than blinking its
            // output for nothing.
            if (!agent || !config || !(*config == wanted.value())) {
                start_agent(wanted.value());
            }
            status.settings_message = std::move(saved_note);
            panel.show_status();
            break;
        }
        case RenderMachineAction::CancelSetup:
            panel.show_status();
            break;
        case RenderMachineAction::ChangeSettings:
            panel.open_setup(render_machine_form(options.stored), {},
                             config.has_value());
            break;
        case RenderMachineAction::StartProcess:
        case RenderMachineAction::StopProcess:
        case RenderMachineAction::RestartProcess:
            if (agent) {
                const auto command =
                    action == RenderMachineAction::StartProcess
                        ? net::ClusterLifecycleCommand::Start
                    : action == RenderMachineAction::StopProcess
                        ? net::ClusterLifecycleCommand::Stop
                        : net::ClusterLifecycleCommand::Restart;
                const auto reply = agent->apply(command);
                status.process = reply.state;
                status.process_message = reply.message;
                next_status = clock::now() + std::chrono::milliseconds{250};
            }
            break;
        case RenderMachineAction::RetryAgent:
            if (config) {
                start_agent(*config);
            }
            break;
        case RenderMachineAction::AllowFirewall:
            if (!firewall_change && !rules.empty()) {
                std::string error;
                firewall_change = ElevatedCommand::start(
                    firewall_rules_command(rules), error);
                status.firewall_message = error;
                status.firewall_busy = firewall_change != nullptr;
            }
            break;
        case RenderMachineAction::ToggleSignIn: {
            const bool wanted = !status.starts_at_sign_in;
            status.sign_in_message =
                set_start_at_sign_in(options.executable, wanted)
                    ? std::string{}
                    : "Windows did not accept the change.";
            status.starts_at_sign_in = starts_at_sign_in(options.executable);
            break;
        }
        case RenderMachineAction::OpenEditor: {
            // Its WAL defaults to the working directory, so that is this
            // user's settings folder rather than wherever Explorer started us.
            std::filesystem::path working = options.executable.parent_path();
            if (!options.preferences_file.empty()) {
                std::error_code ignored;
                const auto folder = options.preferences_file.parent_path();
                std::filesystem::create_directories(folder, ignored);
                if (std::filesystem::is_directory(folder, ignored)) {
                    working = folder;
                }
            }
            const std::string error = start_independent_process(
                options.executable, editor_launch_arguments(options.stored),
                working, options.no_console_window);
            status.editor_message =
                error.empty() ? std::string{}
                              : "The editor did not start: " + error;
            // Not set up as a render machine, nothing here is running, so
            // this window has done its job.
            if (error.empty() && !agent) {
                open = false;
            }
            break;
        }
        case RenderMachineAction::Close:
            open = false;
            break;
        }

        // A render machine's GPU belongs to its output, so a window nobody is
        // touching redraws a few times a second rather than every refresh.
        if (!input_active(input) && !ctx.typing()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        } else if (!input_active(input)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{30});
        }
    }
    if (agent) {
        agent->stop();
    }
    agent.reset();
    return 0;
}

} // namespace mediaserver::app
