#pragma once

#include "render_machine.hpp"
#include "render_machine_system.hpp"

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/persist/settings.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The window a render machine started without a command line shows: a setup
// form on the first launch, and from then on what the machine is doing - the
// render process, who may control it, and whether Windows Firewall lets the
// control machine in.
namespace mediaserver::app {

// The size the window opens at, in logical units.
inline constexpr core::u32 render_machine_window_width = 560U;
inline constexpr core::u32 render_machine_window_height = 700U;

// What the status view shows, as of this frame.
struct RenderMachineStatus final {
    std::string hostname;
    // This machine's IPv4 addresses, for entering it in the Hardware Map by
    // hand when discovery cannot reach the editor.
    std::vector<std::string> addresses;

    bool agent_running{};
    // Why the agent is not running, once it has failed to start.
    std::string agent_error;
    core::u32 node_index{};
    std::string director;
    std::uint16_t control_port{};
    std::string discovery;
    bool password_set{};
    net::ClusterProcessState process{net::ClusterProcessState::Stopped};
    std::string process_message;
    core::u64 accepted_requests{};
    core::u64 refused_requests{};

    bool firewall_supported{};
    FirewallStatus firewall{FirewallStatus::Unknown};
    // An elevated change is waiting for the operator or still running.
    bool firewall_busy{};
    std::string firewall_message;

    bool sign_in_supported{};
    bool starts_at_sign_in{};
    std::string sign_in_message;

    // Set when the last settings saved apply to this run only.
    std::string settings_message;
};

enum class RenderMachineAction : core::u8 {
    None,
    // The form is ready to validate and save: see RenderMachinePanel::form().
    SaveAndStart,
    CancelSetup,
    ChangeSettings,
    StartProcess,
    StopProcess,
    RestartProcess,
    // Try the stored settings again after the agent failed to start.
    RetryAgent,
    AllowFirewall,
    ToggleSignIn,
    // Confirmed: stop the agent and close the window.
    Close,
};

// Where the last frame put each control; empty for one it did not draw.
// Text wraps to the font, so tests click these rather than fixed points.
struct RenderMachinePanelLayout final {
    gui::Rect address;
    gui::Rect port;
    gui::Rect node_index;
    gui::Rect password;
    gui::Rect clear_password;
    gui::Rect control_port;
    gui::Rect show_id;
    gui::Rect save;
    gui::Rect cancel;
    gui::Rect start;
    gui::Rect stop;
    gui::Rect restart;
    gui::Rect retry;
    gui::Rect firewall;
    gui::Rect sign_in;
    gui::Rect change;
    gui::Rect close;
    gui::Rect keep_running;
    gui::Rect stop_and_close;
};

// Pure drawing and interaction over a UiContext, so tests drive it with
// synthetic input and no window.
class RenderMachinePanel final {
public:
    // Shows the setup form filled from `form`. `message` says why it is
    // showing - empty on a first run - and `can_cancel` offers a way back to
    // the settings already running.
    void open_setup(RenderMachineForm form, std::string message,
                    bool can_cancel);
    void show_status() noexcept;
    [[nodiscard]] bool setup_open() const noexcept { return setup_open_; }
    [[nodiscard]] const RenderMachineForm& form() const noexcept {
        return form_;
    }
    // A SaveAndStart the settings refused: the form stays, with the reason.
    void reject_form(RenderMachineFormError error);

    // The window's close button. Asks first, since closing stops the output.
    void request_close() noexcept { confirm_close_ = true; }
    [[nodiscard]] bool confirming_close() const noexcept {
        return confirm_close_;
    }

    [[nodiscard]] RenderMachineAction draw(gui::UiContext& ctx,
                                           const gui::Rect& bounds,
                                           const RenderMachineStatus& status);
    [[nodiscard]] const RenderMachinePanelLayout& layout() const noexcept {
        return layout_;
    }

private:
    [[nodiscard]] RenderMachineAction draw_setup(gui::UiContext& ctx,
                                                 const gui::Rect& bounds);
    [[nodiscard]] RenderMachineAction
    draw_status(gui::UiContext& ctx, const gui::Rect& bounds,
                const RenderMachineStatus& status);
    [[nodiscard]] RenderMachineAction draw_close(gui::UiContext& ctx,
                                                 const gui::Rect& bounds);
    // The region between the header and the footer, scrolled by the wheel
    // when a view is taller than the window. Returns its top, scrolled.
    [[nodiscard]] float begin_content(gui::UiContext& ctx,
                                      const gui::Rect& bounds);
    void end_content(gui::UiContext& ctx, const gui::Rect& bounds,
                     float bottom);

    RenderMachineForm form_{};
    RenderMachinePanelLayout layout_{};
    std::string message_;
    std::optional<RenderMachineFormError> error_;
    bool setup_open_{};
    bool can_cancel_{};
    bool confirm_close_{};
    float scroll_{};
    // How tall the last frame's content was, unscrolled.
    float content_height_{};
};

// Runs the window until it is closed. Returns the process exit code: 0 when
// the operator closed it, 1 when no window could be opened.
[[nodiscard]] int run_render_machine_window(RenderMachineWindowOptions options);

} // namespace mediaserver::app
