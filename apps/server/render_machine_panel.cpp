// The render-machine window's contents: drawing and interaction only, so the
// panel tests drive it with synthetic input and no window, agent, or network.
#include "render_machine_window.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/layout.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace mediaserver::app {
namespace {

constexpr float margin = 20.0F;
constexpr float header_height = 44.0F;
constexpr float footer_height = 58.0F;
constexpr float label_width = 116.0F;
constexpr float row_height = 26.0F;
constexpr float text_line = 18.0F;
constexpr float button_height = 30.0F;

const gui::UiId field_address = gui::ui_id("render-machine.address");
const gui::UiId field_port = gui::ui_id("render-machine.port");
const gui::UiId field_node = gui::ui_id("render-machine.node-index");
const gui::UiId field_password = gui::ui_id("render-machine.password");
const gui::UiId field_control = gui::ui_id("render-machine.control-port");
const gui::UiId field_show = gui::ui_id("render-machine.show-id");

// Lines no wider than `width`, broken between words. A word wider than the
// line gets one to itself rather than being cut.
[[nodiscard]] std::vector<std::string>
wrap(const gui::UiContext& ctx, std::string_view text, float width) {
    std::vector<std::string> lines;
    std::string line;
    while (!text.empty()) {
        const auto space = text.find(' ');
        const std::string_view word = text.substr(0U, space);
        text = space == std::string_view::npos ? std::string_view{}
                                               : text.substr(space + 1U);
        std::string candidate = line;
        if (!candidate.empty()) {
            candidate.push_back(' ');
        }
        candidate.append(word);
        if (!line.empty() && ctx.font_width(candidate) > width) {
            lines.push_back(std::move(line));
            line = std::string{word};
        } else {
            line = std::move(candidate);
        }
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// Wrapped text from `y`; returns the y below it.
float paragraph(gui::UiContext& ctx, float x, float y, float width,
                std::string_view text, gui::UiColor color) {
    for (const std::string& line : wrap(ctx, text, width)) {
        ctx.label(gui::Rect{x, y, width, text_line}, line, color);
        y += text_line;
    }
    return y;
}

// A section title with a rule above it; returns the y below it.
float section(gui::UiContext& ctx, float x, float y, float width,
              std::string_view title) {
    ctx.panel(gui::Rect{x, y, width, 1.0F}, ctx.theme().divider);
    ctx.label(gui::Rect{x, y + 8.0F, width, text_line}, title,
              ctx.theme().text_muted);
    return y + 32.0F;
}

void header(gui::UiContext& ctx, const gui::Rect& bounds,
            std::string_view title, std::string_view aside) {
    const gui::Theme& theme = ctx.theme();
    const gui::Rect band{bounds.x, bounds.y, bounds.width, header_height};
    ctx.panel(band, theme.header);
    ctx.label(gui::Rect{band.x + margin, band.y, band.width - 2.0F * margin,
                        band.height},
              title, theme.text);
    if (!aside.empty()) {
        const float width = ctx.font_width(aside);
        ctx.label(gui::Rect{band.x + band.width - margin - width, band.y,
                            width, band.height},
                  aside, theme.text_muted);
    }
    ctx.panel(gui::Rect{band.x, band.y + band.height - 1.0F, band.width, 1.0F},
              theme.border);
}

[[nodiscard]] gui::Rect footer(gui::UiContext& ctx, const gui::Rect& bounds) {
    const gui::Rect bar{bounds.x, bounds.y + bounds.height - footer_height,
                        bounds.width, footer_height};
    ctx.panel(bar, ctx.theme().header);
    ctx.panel(gui::Rect{bar.x, bar.y, bar.width, 1.0F}, ctx.theme().border);
    return bar;
}

[[nodiscard]] float footer_button_y(const gui::Rect& bar) noexcept {
    return bar.y + (bar.height - button_height) * 0.5F;
}

// A button that can be shown but not pressed, so the layout never jumps.
[[nodiscard]] bool action_button(gui::UiContext& ctx, gui::UiId id,
                                 const gui::Rect& bounds,
                                 std::string_view text, bool enabled = true) {
    if (enabled) {
        return ctx.button(id, bounds, text);
    }
    ctx.panel(bounds, ctx.theme().row);
    const float width = ctx.font_width(text);
    ctx.label(gui::Rect{bounds.x + (bounds.width - width) * 0.5F, bounds.y,
                        width, bounds.height},
              text, ctx.theme().text_muted);
    return false;
}

// A text field with a quiet placeholder while it is empty and not typing.
[[nodiscard]] bool field(gui::UiContext& ctx, gui::UiId id,
                         const gui::Rect& bounds, std::string& value,
                         std::string_view placeholder, bool password = false) {
    const bool changed = password ? ctx.password_field(id, bounds, value)
                                  : ctx.text_field(id, bounds, value);
    if (value.empty() && ctx.focus() != id && !placeholder.empty()) {
        ctx.label(gui::Rect{bounds.x + 6.0F, bounds.y,
                            std::max(bounds.width - 12.0F, 0.0F),
                            bounds.height},
                  placeholder, ctx.theme().text_muted);
    }
    return changed;
}

[[nodiscard]] std::string_view
process_text(net::ClusterProcessState state) noexcept {
    switch (state) {
    case net::ClusterProcessState::Stopped:
        return "RENDER PROCESS STOPPED";
    case net::ClusterProcessState::Starting:
        return "RENDER PROCESS STARTING";
    case net::ClusterProcessState::Running:
        return "RENDER PROCESS RUNNING";
    case net::ClusterProcessState::Stopping:
        return "RENDER PROCESS STOPPING";
    case net::ClusterProcessState::Faulted:
        return "RENDER PROCESS FAULTED";
    }
    return "RENDER PROCESS FAULTED";
}

[[nodiscard]] gui::UiColor process_color(const gui::Theme& theme,
                                         net::ClusterProcessState state) {
    switch (state) {
    case net::ClusterProcessState::Running:
        return theme.good;
    case net::ClusterProcessState::Faulted:
        return theme.warning;
    case net::ClusterProcessState::Stopped:
        return theme.text_muted;
    case net::ClusterProcessState::Starting:
    case net::ClusterProcessState::Stopping:
        break;
    }
    return theme.text;
}

// One "Label   value" row of the status view; returns the y below it.
float fact(gui::UiContext& ctx, float x, float y, float width,
           std::string_view label, std::string_view value,
           gui::UiColor color) {
    ctx.label(gui::Rect{x, y, label_width, text_line}, label,
              ctx.theme().text_muted);
    const float bottom = paragraph(ctx, x + label_width, y,
                                   width - label_width, value, color);
    return std::max(bottom, y + text_line) + 6.0F;
}

// The port a typed node index would get, for the control port placeholder.
[[nodiscard]] std::string automatic_port_text(const std::string& node_index) {
    std::uint32_t index{};
    const char* const begin = node_index.data();
    const char* const end = begin + node_index.size();
    const auto parsed = std::from_chars(begin, end, index);
    if (begin == end || parsed.ec != std::errc{} || parsed.ptr != end ||
        index > persist::maximum_render_machine_node_index) {
        return "automatic";
    }
    return std::to_string(render_machine_default_control_port(index)) +
           " (automatic)";
}

} // namespace

void RenderMachinePanel::open_setup(RenderMachineForm form, std::string message,
                                    bool can_cancel) {
    form_ = std::move(form);
    message_ = std::move(message);
    error_.reset();
    setup_open_ = true;
    can_cancel_ = can_cancel;
    confirm_close_ = false;
    scroll_ = 0.0F;
}

void RenderMachinePanel::show_status() noexcept {
    setup_open_ = false;
    error_.reset();
    scroll_ = 0.0F;
}

void RenderMachinePanel::reject_form(RenderMachineFormError error) {
    error_ = std::move(error);
}

RenderMachineAction RenderMachinePanel::draw(gui::UiContext& ctx,
                                             const gui::Rect& bounds,
                                             const RenderMachineStatus& status) {
    layout_ = {};
    ctx.panel(bounds, ctx.theme().panel);
    if (confirm_close_) {
        return draw_close(ctx, bounds);
    }
    return setup_open_ ? draw_setup(ctx, bounds, status)
                       : draw_status(ctx, bounds, status);
}

float RenderMachinePanel::begin_content(gui::UiContext& ctx,
                                        const gui::Rect& bounds) {
    const gui::Rect content{
        bounds.x, bounds.y + header_height, bounds.width,
        std::max(bounds.height - header_height - footer_height, 0.0F)};
    if (content.contains(ctx.pointer_x(), ctx.pointer_y())) {
        scroll_ -= ctx.wheel() * 36.0F;
    }
    scroll_ = std::clamp(scroll_, 0.0F,
                         std::max(content_height_ - content.height, 0.0F));
    ctx.push_clip(content);
    return content.y + 14.0F - scroll_;
}

void RenderMachinePanel::end_content(gui::UiContext& ctx,
                                     const gui::Rect& bounds, float bottom) {
    ctx.pop_clip();
    content_height_ = bottom + scroll_ - (bounds.y + header_height) + 14.0F;
}

RenderMachineAction
RenderMachinePanel::draw_setup(gui::UiContext& ctx, const gui::Rect& bounds,
                               const RenderMachineStatus& status) {
    const gui::Theme& theme = ctx.theme();
    const gui::UiId focused_before = ctx.focus();
    header(ctx, bounds, "SET UP THIS RENDER MACHINE", {});

    const float x = bounds.x + margin;
    const float width = std::max(bounds.width - 2.0F * margin, 0.0F);
    const float field_x = x + label_width;
    const float field_width = std::max(width - label_width, 0.0F);
    float y = begin_content(ctx, bounds);
    y = paragraph(ctx, x, y, width,
                  "This computer will drive part of the wall. Once it is set "
                  "up, the editor on the control machine starts, stops and "
                  "feeds it, and this window opens straight to its status.",
                  theme.text_muted);
    y = paragraph(ctx, x, y + 6.0F, width,
                  "Is this the control machine? Open Editor starts the "
                  "editor here instead; nothing below is needed for it.",
                  theme.text_muted);
    if (!status.editor_message.empty()) {
        y = paragraph(ctx, x, y + 6.0F, width, status.editor_message,
                      theme.warning);
    }
    if (!message_.empty()) {
        y = paragraph(ctx, x, y + 6.0F, width, message_, theme.warning);
    }
    y += 14.0F;

    bool edited = false;
    y = section(ctx, x, y, width, "CONTROL MACHINE");
    {
        constexpr float port_label = 38.0F;
        constexpr float port_width = 72.0F;
        ctx.label(gui::Rect{x, y, label_width, row_height}, "Override",
                  theme.text);
        layout_.address = gui::Rect{
            field_x, y,
            std::max(field_width - port_label - port_width - 10.0F, 0.0F),
            row_height};
        edited |= field(ctx, field_address, layout_.address,
                        form_.director_address, "Automatic");
        ctx.label(gui::Rect{layout_.address.x + layout_.address.width + 10.0F,
                            y, port_label, row_height},
                  "Port", theme.text);
        layout_.port =
            gui::Rect{x + width - port_width, y, port_width, row_height};
        edited |= field(ctx, field_port, layout_.port, form_.director_port,
                        "45060");
        y += row_height + 6.0F;
        y = paragraph(ctx, field_x, y, field_width,
                      "Found automatically for this show. Leave the address "
                      "empty to discover it on every start, or enter a manual "
                      "address and port. Clear a saved address to use discovery.",
                      theme.text_muted);
        if (form_.director_address.empty() && !status.editor_search.empty()) {
            y = paragraph(ctx, field_x, y, field_width, status.editor_search,
                          theme.text_muted);
        }
    }
    y += 14.0F;

    y = section(ctx, x, y, width, "THIS MACHINE");
    {
        ctx.label(gui::Rect{x, y, label_width, row_height}, "Node index",
                  theme.text);
        constexpr float index_width = 72.0F;
        layout_.node_index = gui::Rect{field_x, y, index_width, row_height};
        edited |= field(ctx, field_node, layout_.node_index, form_.node_index,
                        "Auto");
        ctx.label(gui::Rect{field_x + index_width + 12.0F, y,
                            std::max(field_width - index_width - 12.0F, 0.0F),
                            row_height},
                  "Assigned by the editor; leave blank for automatic",
                  theme.text_muted);
        y += row_height + 10.0F;

        ctx.label(gui::Rect{x, y, label_width, row_height}, "Password",
                  theme.text);
        constexpr float clear_width = 76.0F;
        layout_.password =
            gui::Rect{field_x, y,
                      std::max(field_width - clear_width - 8.0F, 0.0F),
                      row_height};
        if (field(ctx, field_password, layout_.password, form_.password,
                  form_.password_set ? "Set - type to replace" : "None",
                  true)) {
            form_.password_edited = true;
            form_.password_set = !form_.password.empty();
            edited = true;
        }
        layout_.clear_password =
            gui::Rect{x + width - clear_width, y, clear_width, row_height};
        if (action_button(ctx, gui::ui_id("render-machine.password.clear"),
                          layout_.clear_password, "CLEAR",
                          form_.password_set)) {
            form_.password.clear();
            form_.password_edited = true;
            form_.password_set = false;
            edited = true;
        }
        y += row_height + 6.0F;
        y = paragraph(ctx, field_x, y, field_width,
                      form_.password_set
                          ? "The render node password from File > Preferences "
                            "in the editor on the control machine."
                          : "No password: any Media Server on this network "
                            "can start and stop this machine. Enter the "
                            "render node password if the control machine's "
                            "Preferences has one.",
                      form_.password_set ? theme.text_muted : theme.warning);
    }
    y += 14.0F;

    y = section(ctx, x, y, width, "ADVANCED");
    {
        ctx.label(gui::Rect{x, y, label_width, row_height}, "Control port",
                  theme.text);
        layout_.control_port = gui::Rect{field_x, y, 140.0F, row_height};
        edited |= field(ctx, field_control, layout_.control_port,
                        form_.control_port,
                        automatic_port_text(form_.node_index));
        y += row_height + 10.0F;
        ctx.label(gui::Rect{x, y, label_width, row_height}, "Show ID",
                  theme.text);
        layout_.show_id = gui::Rect{field_x, y, field_width, row_height};
        edited |= field(ctx, field_show, layout_.show_id, form_.show_id,
                        "Automatic - use the discovered editor's show");
        y += row_height + 6.0F;
        y = paragraph(ctx, field_x, y, field_width,
                      "Blank values use the assigned node's control port "
                      "and the discovered editor's show. A show ID filters "
                      "which editor this machine joins.",
                      theme.text_muted);
    }
    if (edited) {
        error_.reset();
    }
    if (error_) {
        y = paragraph(ctx, x, y + 10.0F, width, error_->message, theme.warning);
    }
    end_content(ctx, bounds, y);

    // Tab and Shift+Tab walk the fields; Enter in any of them saves.
    constexpr std::array<gui::UiId, 6U> order{field_address, field_port,
                                              field_node,    field_password,
                                              field_control, field_show};
    const auto focused = std::find(order.begin(), order.end(), focused_before);
    if (focused != order.end() && ctx.focus() == gui::UiId{}) {
        if (ctx.key_pressed(gui::Key::Tab)) {
            const std::size_t at =
                static_cast<std::size_t>(focused - order.begin());
            const std::size_t next =
                ctx.shift_held() ? (at + order.size() - 1U) % order.size()
                                 : (at + 1U) % order.size();
            const std::array<const std::string*, 6U> values{
                &form_.director_address, &form_.director_port,
                &form_.node_index,       &form_.password,
                &form_.control_port,     &form_.show_id};
            ctx.focus_text_field_selected(order[next], *values[next]);
        } else if (ctx.key_pressed(gui::Key::Enter)) {
            return RenderMachineAction::SaveAndStart;
        }
    }

    const gui::Rect bar = footer(ctx, bounds);
    const float button_y = footer_button_y(bar);
    constexpr float save_width = 156.0F;
    layout_.save = gui::Rect{bar.x + bar.width - margin - save_width, button_y,
                             save_width, button_height};
    if (ctx.button(gui::ui_id("render-machine.save"), layout_.save,
                   "SAVE AND START")) {
        return RenderMachineAction::SaveAndStart;
    }
    layout_.open_editor =
        gui::Rect{bar.x + margin, button_y, 140.0F, button_height};
    if (ctx.button(gui::ui_id("render-machine.open-editor"),
                   layout_.open_editor, "OPEN EDITOR")) {
        return RenderMachineAction::OpenEditor;
    }
    if (can_cancel_) {
        layout_.cancel = gui::Rect{layout_.save.x - 10.0F - 100.0F, button_y,
                                   100.0F, button_height};
        if (ctx.button(gui::ui_id("render-machine.cancel"), layout_.cancel,
                       "CANCEL")) {
            return RenderMachineAction::CancelSetup;
        }
    }
    return RenderMachineAction::None;
}

RenderMachineAction
RenderMachinePanel::draw_status(gui::UiContext& ctx, const gui::Rect& bounds,
                                const RenderMachineStatus& status) {
    const gui::Theme& theme = ctx.theme();
    header(ctx, bounds,
           "RENDER MACHINE - NODE " + std::to_string(status.node_index),
           status.hostname);

    const float x = bounds.x + margin;
    const float width = std::max(bounds.width - 2.0F * margin, 0.0F);
    RenderMachineAction action = RenderMachineAction::None;
    float y = begin_content(ctx, bounds);

    if (!status.settings_message.empty()) {
        y = paragraph(ctx, x, y, width, status.settings_message,
                      theme.warning);
        y += 10.0F;
    }
    if (!status.editor_message.empty()) {
        y = paragraph(ctx, x, y, width, status.editor_message, theme.warning);
        y += 10.0F;
    }
    if (!status.agent_running) {
        ctx.label(gui::Rect{x, y, width, row_height},
                  status.waiting_for_identity ? "Waiting for editor" : "Not running",
                  status.waiting_for_identity ? theme.caution : theme.warning);
        y += row_height + 4.0F;
        y = paragraph(ctx, x, y, width,
                      status.waiting_for_identity ? status.editor_search : status.agent_error, theme.text);
        y += 12.0F;
        layout_.retry = gui::Rect{x, y, 120.0F, button_height};
        if (ctx.button(gui::ui_id("render-machine.retry"), layout_.retry,
                       "TRY AGAIN")) {
            action = RenderMachineAction::RetryAgent;
        }
        y += button_height + 14.0F;
    } else {
        // The one line to read from across the room.
        const gui::UiColor state_color = process_color(theme, status.process);
        ctx.panel(gui::Rect{x, y + 8.0F, 10.0F, 10.0F}, state_color);
        ctx.label(gui::Rect{x + 18.0F, y, width - 18.0F, row_height},
                  process_text(status.process), state_color);
        constexpr float control_width = 84.0F;
        const float control_y = y - 2.0F;
        const bool running =
            status.process == net::ClusterProcessState::Running;
        if (running) {
            layout_.stop = gui::Rect{x + width - control_width, control_y,
                                     control_width, button_height};
            if (ctx.button(gui::ui_id("render-machine.stop"), layout_.stop,
                           "STOP")) {
                action = RenderMachineAction::StopProcess;
            }
            layout_.restart =
                gui::Rect{layout_.stop.x - 8.0F - control_width, control_y,
                          control_width, button_height};
            if (ctx.button(gui::ui_id("render-machine.restart"),
                           layout_.restart, "RESTART")) {
                action = RenderMachineAction::RestartProcess;
            }
        } else {
            layout_.start = gui::Rect{x + width - control_width, control_y,
                                      control_width, button_height};
            if (ctx.button(gui::ui_id("render-machine.start"), layout_.start,
                           "START")) {
                action = RenderMachineAction::StartProcess;
            }
        }
        y += row_height + 6.0F;
        if (!running && !status.process_message.empty()) {
            y = paragraph(ctx, x + 18.0F, y, width - 18.0F,
                          status.process_message, theme.text_muted);
        }
        y = paragraph(ctx, x + 18.0F, y, width - 18.0F,
                      "Output Monitor in the editor on the control machine "
                      "starts and stops it too.",
                      theme.text_muted);
        y += 14.0F;

        y = fact(ctx, x, y, width, "Control machine", status.director,
                 theme.text);
        y = fact(ctx, x, y, width, "Show", status.show_id, theme.text);
        if (status.director == "Automatic discovery") {
            y = paragraph(ctx, x, y, width, status.editor_search, theme.text_muted);
        }
        std::string control = "TCP " + std::to_string(status.control_port);
        control += status.discovery.starts_with("udp/")
                       ? ", found by editors on UDP " +
                             status.discovery.substr(4U)
                       : " - discovery " + status.discovery;
        y = fact(ctx, x, y, width, "Listening on", control, theme.text);
        y = fact(ctx, x, y, width, "Password",
                 status.password_set
                     ? "Set"
                     : "None - any Media Server on this network can control "
                       "this machine",
                 status.password_set ? theme.text : theme.warning);
        std::string requests =
            std::to_string(status.accepted_requests) + " accepted, " +
            std::to_string(status.refused_requests) + " refused";
        if (status.refused_requests != 0U) {
            requests += " - a refused request had the wrong password";
        }
        y = fact(ctx, x, y, width, "Requests", requests,
                 status.refused_requests != 0U ? theme.warning : theme.text);
        std::string addresses;
        for (const std::string& address : status.addresses) {
            if (!addresses.empty()) {
                addresses += ", ";
            }
            addresses += address;
        }
        y = fact(ctx, x, y, width, "This machine",
                 addresses.empty() ? std::string{"no network address"}
                                   : addresses,
                 theme.text);
        y = paragraph(ctx, x, y + 2.0F, width,
                      "In the editor, open Hardware Map: this machine is "
                      "listed under DETECTED ON NETWORK. Drag it onto the "
                      "map, or add a Render Server with the address and "
                      "port above.",
                      theme.text_muted);
        y += 14.0F;

        if (status.firewall_supported) {
            y = section(ctx, x, y, width, "WINDOWS FIREWALL");
            std::string_view firewall_text;
            gui::UiColor firewall_color = theme.text_muted;
            switch (status.firewall) {
            case FirewallStatus::RulesPresent:
                firewall_text = "Rules let the control machine in.";
                firewall_color = theme.good;
                break;
            case FirewallStatus::RulesMissing:
                firewall_text =
                    "No Media Server rule for these ports. Unless Windows "
                    "asked about Media Server and you allowed it, the editor "
                    "can neither find nor control this machine.";
                firewall_color = theme.warning;
                break;
            case FirewallStatus::Off:
                firewall_text = "Windows Firewall is off on this network.";
                break;
            case FirewallStatus::Unknown:
                firewall_text = "Could not read the Windows Firewall rules.";
                break;
            }
            y = paragraph(ctx, x, y, width, firewall_text, firewall_color);
            if (!status.firewall_message.empty()) {
                y = paragraph(ctx, x, y + 2.0F, width, status.firewall_message,
                              theme.text);
            }
            if (status.firewall != FirewallStatus::RulesPresent &&
                status.firewall != FirewallStatus::Off) {
                y += 8.0F;
                layout_.firewall = gui::Rect{x, y, 230.0F, button_height};
                if (action_button(ctx, gui::ui_id("render-machine.firewall"),
                                  layout_.firewall,
                                  status.firewall_busy
                                      ? "WAITING FOR WINDOWS..."
                                      : "ALLOW THROUGH FIREWALL",
                                  !status.firewall_busy)) {
                    action = RenderMachineAction::AllowFirewall;
                }
                y += button_height + 6.0F;
                y = paragraph(ctx, x, y, width,
                              "Windows asks for an administrator first, then "
                              "adds inbound rules for exactly these ports.",
                              theme.text_muted);
            }
            y += 14.0F;
        }
    }

    if (status.sign_in_supported) {
        y = section(ctx, x, y, width, "STARTUP");
        bool starts = status.starts_at_sign_in;
        layout_.sign_in = gui::Rect{x, y, width, row_height};
        if (ctx.checkbox(gui::ui_id("render-machine.sign-in"), layout_.sign_in,
                         starts,
                         "Start this render machine when I sign in to "
                         "Windows")) {
            action = RenderMachineAction::ToggleSignIn;
        }
        y += row_height + 4.0F;
        if (!status.sign_in_message.empty()) {
            y = paragraph(ctx, x, y, width, status.sign_in_message,
                          theme.warning);
        }
    }
    end_content(ctx, bounds, y);

    const gui::Rect bar = footer(ctx, bounds);
    const float button_y = footer_button_y(bar);
    layout_.change =
        gui::Rect{bar.x + margin, button_y, 170.0F, button_height};
    if (ctx.button(gui::ui_id("render-machine.change"), layout_.change,
                   "CHANGE SETTINGS")) {
        action = RenderMachineAction::ChangeSettings;
    }
    layout_.open_editor =
        gui::Rect{layout_.change.x + layout_.change.width + 10.0F, button_y,
                  140.0F, button_height};
    if (ctx.button(gui::ui_id("render-machine.open-editor"),
                   layout_.open_editor, "OPEN EDITOR")) {
        action = RenderMachineAction::OpenEditor;
    }
    layout_.close = gui::Rect{bar.x + bar.width - margin - 100.0F, button_y,
                              100.0F, button_height};
    if (ctx.button(gui::ui_id("render-machine.close"), layout_.close,
                   "CLOSE")) {
        confirm_close_ = true;
    }
    return action;
}

RenderMachineAction RenderMachinePanel::draw_close(gui::UiContext& ctx,
                                                   const gui::Rect& bounds) {
    const gui::Theme& theme = ctx.theme();
    header(ctx, bounds, "CLOSE THIS RENDER MACHINE?", {});
    const float x = bounds.x + margin;
    const float width = std::max(bounds.width - 2.0F * margin, 0.0F);
    static_cast<void>(paragraph(
        ctx, x, bounds.y + header_height + 16.0F, width,
        "Closing stops the render process, so this machine's part of the "
        "wall goes dark, and Output Monitor cannot start it again until this "
        "window is opened again.",
        theme.text));
    const gui::Rect bar = footer(ctx, bounds);
    const float button_y = footer_button_y(bar);
    constexpr float stop_width = 156.0F;
    layout_.stop_and_close = gui::Rect{bar.x + bar.width - margin - stop_width,
                                       button_y, stop_width, button_height};
    if (ctx.button(gui::ui_id("render-machine.close.confirm"),
                   layout_.stop_and_close, "STOP AND CLOSE")) {
        return RenderMachineAction::Close;
    }
    layout_.keep_running = gui::Rect{layout_.stop_and_close.x - 10.0F - 140.0F,
                                     button_y, 140.0F, button_height};
    if (ctx.button(gui::ui_id("render-machine.close.cancel"),
                   layout_.keep_running, "KEEP RUNNING") ||
        ctx.key_pressed(gui::Key::Escape)) {
        confirm_close_ = false;
    }
    return RenderMachineAction::None;
}

} // namespace mediaserver::app
