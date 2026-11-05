#include "render_machine_window.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace app = mediaserver::app;
namespace gui = mediaserver::gui;
namespace net = mediaserver::net;
namespace persist = mediaserver::persist;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PANEL_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A panel in a window-sized frame, driven with synthetic input against a
// fontless atlas: every solid fill and every interaction is real, only the
// glyphs are missing.
struct Harness final {
    static constexpr gui::Rect bounds{
        0.0F, 0.0F, static_cast<float>(app::render_machine_window_width),
        static_cast<float>(app::render_machine_window_height)};

    gui::FontAtlas font = gui::FontAtlas::empty();
    gui::InputState input;
    gui::UiDrawList list;
    gui::UiContext ctx{input, list, font};
    app::RenderMachinePanel panel;
    app::RenderMachineStatus status{};
    float x{};
    float y{};

    app::RenderMachineAction frame(bool down = false,
                                   std::string_view text = {},
                                   std::initializer_list<gui::Key> keys = {}) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.add_text(text);
        for (const gui::Key key : keys) {
            input.add_key(key);
        }
        ctx.new_frame();
        return panel.draw(ctx, bounds, status);
    }

    // Press and release over the middle of `target`.
    app::RenderMachineAction click(gui::Rect target) {
        PANEL_CHECK(target.width > 0.0F && target.height > 0.0F);
        x = target.x + target.width * 0.5F;
        y = target.y + target.height * 0.5F;
        static_cast<void>(frame(false));
        static_cast<void>(frame(true));
        return frame(false);
    }

    app::RenderMachineAction type(std::string_view text) {
        return frame(false, text);
    }

    app::RenderMachineAction key(gui::Key key) {
        return frame(false, {}, {key});
    }
};

[[nodiscard]] app::RenderMachineStatus running_status() {
    app::RenderMachineStatus status{};
    status.hostname = "RENDER-03";
    status.addresses = {"192.168.1.31"};
    status.agent_running = true;
    status.node_index = 3U;
    status.director = "192.168.1.20:45060";
    status.control_port = 47'843U;
    status.discovery = "udp/47839";
    status.process = net::ClusterProcessState::Running;
    status.firewall_supported = true;
    status.firewall = app::FirewallStatus::RulesMissing;
    status.sign_in_supported = true;
    return status;
}

void a_first_run_is_filled_in_with_the_keyboard_and_saved_with_enter() {
    Harness harness;
    harness.panel.open_setup(app::render_machine_form(persist::Preferences{}),
                             {}, false);
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.setup_open());
    // Nothing to go back to on a first run.
    PANEL_CHECK(harness.panel.layout().cancel.width == 0.0F);

    static_cast<void>(harness.click(harness.panel.layout().address));
    static_cast<void>(harness.type("192.168.1.20"));
    // Tab moves on to the port with its value selected, so typing replaces
    // it; Tab again reaches the node index.
    static_cast<void>(harness.key(gui::Key::Tab));
    static_cast<void>(harness.type("45100"));
    static_cast<void>(harness.key(gui::Key::Tab));
    static_cast<void>(harness.type("3"));
    PANEL_CHECK(harness.panel.form().director_address == "192.168.1.20");
    PANEL_CHECK(harness.panel.form().director_port == "45100");
    PANEL_CHECK(harness.panel.form().node_index == "3");

    PANEL_CHECK(harness.key(gui::Key::Enter) ==
                app::RenderMachineAction::SaveAndStart);
    const auto saved = app::apply_render_machine_form(harness.panel.form(),
                                                      persist::Preferences{});
    PANEL_CHECK(saved);
    PANEL_CHECK(saved.value().render_machine_director == "192.168.1.20:45100");
    PANEL_CHECK(saved.value().render_machine_node_index == 3U);
}

void a_typed_password_is_marked_edited_and_clear_removes_a_stored_one() {
    Harness harness;
    auto form = app::render_machine_form(persist::Preferences{});
    form.password_set = true;
    harness.panel.open_setup(form, {}, true);
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.layout().cancel.width > 0.0F);

    // CLEAR forgets the stored password without anything being typed.
    static_cast<void>(harness.click(harness.panel.layout().clear_password));
    PANEL_CHECK(!harness.panel.form().password_set);
    PANEL_CHECK(harness.panel.form().password_edited);
    PANEL_CHECK(harness.panel.form().password.empty());

    static_cast<void>(harness.click(harness.panel.layout().password));
    static_cast<void>(harness.type("hunter2"));
    PANEL_CHECK(harness.panel.form().password == "hunter2");
    PANEL_CHECK(harness.panel.form().password_set);

    PANEL_CHECK(harness.click(harness.panel.layout().cancel) ==
                app::RenderMachineAction::CancelSetup);
}

void the_save_button_saves_and_a_refusal_keeps_the_form_open() {
    Harness harness;
    harness.panel.open_setup(app::render_machine_form(persist::Preferences{}),
                             {}, false);
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().save) ==
                app::RenderMachineAction::SaveAndStart);
    // An empty address is automatic discovery, including when the preview
    // displays an editor's current address; saving must never pin that IP.
    harness.status.discovered_editor = "192.0.2.31:45217";
    harness.status.editor_search = "Editor found: 192.0.2.31:45217";
    static_cast<void>(harness.frame());
    const auto automatic = app::apply_render_machine_form(
        harness.panel.form(), persist::Preferences{});
    PANEL_CHECK(automatic);
    PANEL_CHECK(automatic.value().render_machine_director == "auto");
    PANEL_CHECK(automatic.value().render_machine_node_index == net::automatic_node_index);
    PANEL_CHECK(automatic.value().render_machine_show_id.empty());
    PANEL_CHECK(automatic.value().render_machine_node_id.size() == 32U);
    static_cast<void>(harness.click(harness.panel.layout().address));
    static_cast<void>(harness.type("bad address"));
    const auto refused = app::apply_render_machine_form(
        harness.panel.form(), persist::Preferences{});
    PANEL_CHECK(!refused);
    harness.panel.reject_form(refused.error());
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.setup_open());
}

void the_status_view_offers_the_controls_that_fit_the_process_state() {
    Harness harness;
    harness.status = running_status();
    harness.panel.show_status();
    static_cast<void>(harness.frame());
    const auto& layout = harness.panel.layout();
    PANEL_CHECK(layout.stop.width > 0.0F);
    PANEL_CHECK(layout.restart.width > 0.0F);
    PANEL_CHECK(layout.start.width == 0.0F);
    PANEL_CHECK(harness.click(harness.panel.layout().stop) ==
                app::RenderMachineAction::StopProcess);
    PANEL_CHECK(harness.click(harness.panel.layout().restart) ==
                app::RenderMachineAction::RestartProcess);

    harness.status.process = net::ClusterProcessState::Faulted;
    harness.status.process_message = "process exited with failure";
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.layout().stop.width == 0.0F);
    PANEL_CHECK(harness.click(harness.panel.layout().start) ==
                app::RenderMachineAction::StartProcess);

    PANEL_CHECK(harness.click(harness.panel.layout().change) ==
                app::RenderMachineAction::ChangeSettings);
}

void the_firewall_button_shows_only_while_a_rule_is_missing() {
    Harness harness;
    harness.status = running_status();
    harness.panel.show_status();
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().firewall) ==
                app::RenderMachineAction::AllowFirewall);

    // While Windows is asking, the button stays where it is but does nothing.
    harness.status.firewall_busy = true;
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().firewall) ==
                app::RenderMachineAction::None);

    harness.status.firewall_busy = false;
    harness.status.firewall = app::FirewallStatus::RulesPresent;
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.layout().firewall.width == 0.0F);
    harness.status.firewall = app::FirewallStatus::Off;
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.layout().firewall.width == 0.0F);

    PANEL_CHECK(harness.click(harness.panel.layout().sign_in) ==
                app::RenderMachineAction::ToggleSignIn);
}

void an_agent_that_did_not_start_offers_to_try_again() {
    Harness harness;
    harness.status = running_status();
    harness.status.agent_running = false;
    harness.status.agent_error = "Control port TCP 47843 is already in use.";
    harness.panel.show_status();
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.layout().stop.width == 0.0F);
    PANEL_CHECK(harness.panel.layout().firewall.width == 0.0F);
    PANEL_CHECK(harness.click(harness.panel.layout().retry) ==
                app::RenderMachineAction::RetryAgent);
}

void closing_a_running_machine_asks_first() {
    Harness harness;
    harness.status = running_status();
    harness.panel.show_status();
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().close) ==
                app::RenderMachineAction::None);
    PANEL_CHECK(harness.panel.confirming_close());
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().keep_running) ==
                app::RenderMachineAction::None);
    PANEL_CHECK(!harness.panel.confirming_close());

    // The window's own close button asks the same question.
    harness.panel.request_close();
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().stop_and_close) ==
                app::RenderMachineAction::Close);
}

struct Case final {
    const char* name;
    void (*function)();
};

void open_editor_is_offered_on_the_setup_form_and_the_status_view() {
    Harness harness;
    harness.panel.open_setup(app::render_machine_form(persist::Preferences{}),
                             {}, false);
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.click(harness.panel.layout().open_editor) ==
                app::RenderMachineAction::OpenEditor);
    // A failed launch is said where the operator just clicked.
    harness.status.editor_message = "The editor did not start: access denied";
    static_cast<void>(harness.frame());
    PANEL_CHECK(harness.panel.setup_open());

    harness.status = running_status();
    harness.panel.show_status();
    static_cast<void>(harness.frame());
    const auto& layout = harness.panel.layout();
    PANEL_CHECK(layout.open_editor.width > 0.0F);
    PANEL_CHECK(layout.open_editor.x >= layout.change.x + layout.change.width);
    PANEL_CHECK(layout.open_editor.x + layout.open_editor.width <=
                layout.close.x);
    PANEL_CHECK(harness.click(harness.panel.layout().open_editor) ==
                app::RenderMachineAction::OpenEditor);
}

constexpr std::array<Case, 8U> cases{{
    {"a first run is filled in with the keyboard and saved with Enter",
     &a_first_run_is_filled_in_with_the_keyboard_and_saved_with_enter},
    {"a typed password is marked edited and CLEAR removes a stored one",
     &a_typed_password_is_marked_edited_and_clear_removes_a_stored_one},
    {"the save button saves and a refusal keeps the form open",
     &the_save_button_saves_and_a_refusal_keeps_the_form_open},
    {"the status view offers the controls that fit the process state",
     &the_status_view_offers_the_controls_that_fit_the_process_state},
    {"the firewall button shows only while a rule is missing",
     &the_firewall_button_shows_only_while_a_rule_is_missing},
    {"an agent that did not start offers to try again",
     &an_agent_that_did_not_start_offers_to_try_again},
    {"closing a running machine asks first",
     &closing_a_running_machine_asks_first},
    {"open editor is offered on the setup form and the status view",
     &open_editor_is_offered_on_the_setup_form_and_the_status_view},
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
