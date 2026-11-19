#include <mediaserver/editor/preferences_panel.hpp>

#include "workspace_panel_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace panel_helpers;

namespace {

// The Preferences dialog's fields, in the order they are laid out. Naming the
// members once keeps the layout, the hint text, and the override notice from
// drifting apart as fields are added.
struct PreferencesField final {
  std::string_view label;
  std::string_view hint;
  std::string EditorPreferences::*value;
  bool EditorPreferencesState::*overridden;
};

[[nodiscard]] const std::array<PreferencesField, 4U> &preferences_fields() {
  static const std::array<PreferencesField, 4U> fields{{
      {"Director listen", "ADDRESS:PORT render nodes connect to",
       &EditorPreferences::director_listen,
       &EditorPreferencesState::director_listen_overridden},
      {"Show ID", "32 hexadecimal characters, same on every node",
       &EditorPreferences::show_id,
       &EditorPreferencesState::show_id_overridden},
      {"Director epoch", "32 hexadecimal characters",
       &EditorPreferences::director_epoch,
       &EditorPreferencesState::director_epoch_overridden},
      {"Write-ahead log", "Director recovery journal",
       &EditorPreferences::wal_path,
       &EditorPreferencesState::wal_path_overridden},
  }};
  return fields;
}

constexpr float preferences_card_height = 714.0F;

} // namespace

void PreferencesPanel::open(const EditorPreferences &stored) {
  draft_ = stored;
  password_draft_.clear();
  password_edited_ = false;
  scroll_.reset();
  open_ = true;
}

void PreferencesPanel::close() noexcept { open_ = false; }

void PreferencesPanel::set_proxy_cache_directory(std::string directory) {
  if (open_) {
    draft_.proxy_cache_directory = std::move(directory);
  }
}

PreferencesFrame PreferencesPanel::draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        const EditorPreferencesState &state) {
  PreferencesFrame result{};
  if (!open_) {
    return result;
  }
  const gui::Metrics &m = ctx.theme().metrics;
  const ModalCard modal =
      begin_modal(ctx, bounds, preferences_card_height, "PREFERENCES");

  // An empty field shows what belongs in it, until it has the keyboard.
  const auto hint = [&](gui::UiId id, const gui::Rect &field,
                        const std::string &value, std::string_view text) {
    if (value.empty() && ctx.focus() != id) {
      ctx.label(gui::inset(field, m.gap, 0.0F), text, ctx.theme().text_muted);
    }
  };

  gui::FormLayout form{modal.body, m, scroll_.begin(ctx, modal.body)};
  for (const PreferencesField &field : preferences_fields()) {
    const gui::FormRow row = form.row();
    ctx.label(row.label, field.label, ctx.theme().text);
    const gui::UiId field_id = gui::ui_id(id_, field.label);
    static_cast<void>(ctx.text_field(field_id, row.field, draft_.*(field.value)));
    hint(field_id, row.field, draft_.*(field.value), field.hint);
    // Naming what the process is really using matters more than what is
    // stored: without it, an argument-driven launch looks like the settings
    // are being ignored at random. The line is kept either way, so a field
    // gaining an override does not push the rest of the form down.
    const gui::Rect note = form.note();
    if (state.*(field.overridden)) {
      std::string text{"in use this run: "};
      text.append(state.active.*(field.value));
      text.append("  (command line)");
      ctx.label(note, text, ctx.theme().warning);
    }
  }

  // ------------------------------------------------------- render nodes
  // The password Output Monitor controls render machines with. It is typed,
  // never shown back, and applies the moment it is saved.
  static_cast<void>(gui::draw_form_section(ctx, form, "RENDER NODES"));
  {
    const gui::FormRow row = form.row();
    ctx.label(row.label, "Password", ctx.theme().text);
    gui::Rect input = row.field;
    const gui::Rect clear = gui::cut_right(input, m.button_width);
    static_cast<void>(gui::cut_right(input, m.gap));
    const gui::UiId password_id = gui::ui_id(id_, "preferences.render-node-password");
    if (ctx.password_field(password_id, input, password_draft_)) {
      password_edited_ = true;
      draft_.render_node_password = !password_draft_.empty();
    }
    hint(password_id, input, password_draft_,
         draft_.render_node_password ? "Set - type to replace" : "None");
    const bool clearable = draft_.render_node_password;
    if (preferences_button(ctx, gui::ui_id(id_, "preferences.render-node-password.clear"),
                           clear, "CLEAR", clearable)) {
      password_draft_.clear();
      password_edited_ = true;
      draft_.render_node_password = false;
    }
    ctx.tooltip(row.label,
                "Give each render machine the same password in its setup "
                "window. Applies when saved.");
    // The line is kept either way, so setting a password does not pull the
    // rest of the form up under the pointer.
    const gui::Rect note = form.note();
    if (state.render_node_password_overridden || !draft_.render_node_password) {
      ctx.label(note,
                state.render_node_password_overridden ? "Set on the command line"
                                                      : "No password",
                ctx.theme().warning);
      ctx.tooltip(note, state.render_node_password_overridden
                            ? "This run uses the password given on the "
                              "command line."
                            : "Any Pixelsoup on this network can start "
                              "and stop render machines.");
    }
  }

  // ------------------------------------------------------------- proxies
  static_cast<void>(gui::draw_form_section(ctx, form, "PROXIES"));
  static_cast<void>(ctx.checkbox(gui::ui_id(id_, "preferences.proxy.next-to-source"),
                                 form.control(),
                                 draft_.proxy_cache_next_to_source,
                                 "Store next to source media"));
  {
    const gui::FormRow row = form.row();
    ctx.label(row.label, "Cache folder", ctx.theme().text);
    gui::Rect input = row.field;
    const gui::Rect browse = gui::cut_right(input, m.button_width);
    static_cast<void>(gui::cut_right(input, m.gap));
    const bool custom_folder_enabled = !draft_.proxy_cache_next_to_source;
    if (custom_folder_enabled) {
      const gui::UiId folder_id = gui::ui_id(id_, "preferences.proxy.cache-directory");
      static_cast<void>(
          ctx.text_field(folder_id, input, draft_.proxy_cache_directory));
      hint(folder_id, input, draft_.proxy_cache_directory,
           "Built-in per-user folder");
    } else {
      ctx.panel(input, ctx.theme().track);
      ctx.label(gui::inset(input, m.gap, 0.0F),
                "Next to each source", ctx.theme().text_muted);
    }
    if (preferences_button(ctx, gui::ui_id(id_, "preferences.proxy.browse"), browse,
                           "BROWSE", custom_folder_enabled)) {
      result.browse_proxy_cache = draft_.proxy_cache_directory;
    }
  }

  // A labelled slider with its exact value written after it. `position` is
  // what the track moves; the caller turns it back into `shown`'s units.
  constexpr float slider_value_width = 92.0F;
  constexpr float slider_track_height = 12.0F;
  const auto slider_row = [&](std::string_view label, std::string_view id,
                              float &position, float minimum, float maximum,
                              core::u32 shown, std::string_view suffix) {
    const gui::FormRow row = form.row();
    ctx.label(row.label, label, ctx.theme().text);
    gui::Rect track_row = row.field;
    const gui::Rect value =
        gui::cut_right(track_row, slider_value_width);
    const bool changed = ctx.slider(
        gui::ui_id(id_, id),
        gui::centered(track_row, track_row.width, slider_track_height),
        position, minimum, maximum);
    std::string exact = std::to_string(shown);
    exact.push_back(' ');
    exact.append(suffix);
    ctx.label(gui::inset(value, gui::Padding{m.gap, 0.0F, 0.0F, 0.0F}), exact,
              ctx.theme().text_muted);
    return changed;
  };
  const auto integer_slider_row = [&](std::string_view label,
                                      std::string_view id, core::u32 &value,
                                      core::u32 minimum, core::u32 maximum,
                                      std::string_view suffix) {
    float position = static_cast<float>(value);
    if (slider_row(label, id, position, static_cast<float>(minimum),
                   static_cast<float>(maximum), value, suffix)) {
      value = std::clamp(static_cast<core::u32>(std::lround(position)), minimum,
                         maximum);
    }
  };
  const auto power_of_two_slider_row = [&](std::string_view label,
                                           std::string_view id,
                                           core::u32 &value, core::u32 base,
                                           core::u32 maximum,
                                           std::string_view suffix) {
    const float maximum_power =
        std::log2(static_cast<float>(maximum) / static_cast<float>(base));
    float power = std::log2(static_cast<float>(std::max(value, base)) /
                            static_cast<float>(base));
    if (slider_row(label, id, power, 0.0F, maximum_power, value, suffix)) {
      const auto exponent = static_cast<core::u32>(std::lround(power));
      value = std::min(base << exponent, maximum);
    }
  };

  power_of_two_slider_row("Cache limit", "preferences.proxy.cache-limit",
                          draft_.proxy_cache_gibibytes, 1U, 1'024U, "GiB");

  {
    using Priority = EditorPreferences::TranscodePriority;
    struct PriorityChoice final {
      std::string_view key;
      std::string_view label;
      Priority value;
    };
    constexpr std::array<PriorityChoice, 3U> priorities{{
        {"preferences.proxy.priority.background", "BACKGROUND",
         Priority::Background},
        {"preferences.proxy.priority.balanced", "BALANCED", Priority::Balanced},
        {"preferences.proxy.priority.fast", "FAST", Priority::Fast},
    }};
    const gui::FormRow row = form.row();
    ctx.label(row.label, "Transcode priority", ctx.theme().text);
    for (std::size_t index = 0U; index < priorities.size(); ++index) {
      const PriorityChoice &choice = priorities[index];
      if (preferences_button(
              ctx, gui::ui_id(id_, choice.key),
              gui::column(row.field, index, priorities.size(), m.gap),
              choice.label, draft_.proxy_transcode_priority != choice.value)) {
        draft_.proxy_transcode_priority = choice.value;
      }
    }
  }

  {
    const gui::FormRow row = form.row();
    result.clear_proxy_cache = preferences_button(
        ctx, gui::ui_id(id_, "preferences.proxy.clear"), row.label, "CLEAR CACHE",
        true);
    ctx.label(gui::inset(row.field, gui::Padding{m.gap, 0.0F, 0.0F, 0.0F}),
              "Clears the cache currently in use, not this unsaved draft.",
              ctx.theme().text_muted);
  }

  // ----------------------------------------------------- output playback
  ctx.tooltip(gui::draw_form_section(ctx, form, "OUTPUT PLAYBACK").heading,
              "More buffering smooths output at the cost of latency and "
              "memory.");
  struct BufferPreset final {
    std::string_view key;
    std::string_view label;
    core::u32 buffer;
    core::u32 read_ahead;
  };
  constexpr std::array<BufferPreset, 4U> presets{{
      {"low", "LOW LATENCY", 3U, 3U},
      {"balanced", "BALANCED", 8U, 8U},
      {"smooth", "SMOOTH", 12U, 12U},
      {"reliable", "RELIABLE", 24U, 24U},
  }};
  const gui::Rect preset_row = form.control();
  for (std::size_t index = 0U; index < presets.size(); ++index) {
    const BufferPreset &preset = presets[index];
    const bool selected = draft_.output_buffer_frames == preset.buffer &&
                          draft_.output_read_ahead_frames == preset.read_ahead;
    if (preferences_button(ctx, gui::ui_id(id_, preset.key),
                           gui::column(preset_row, index, presets.size(), m.gap),
                           preset.label, !selected)) {
      draft_.output_buffer_frames = preset.buffer;
      draft_.output_read_ahead_frames = preset.read_ahead;
    }
  }

  integer_slider_row("Output buffer", "preferences.playback.buffer",
                     draft_.output_buffer_frames, 0U, 60U, "frames");
  integer_slider_row("Read-ahead", "preferences.playback.read-ahead",
                     draft_.output_read_ahead_frames, 1U, 60U, "frames");
  power_of_two_slider_row("Decoded cache", "preferences.playback.cache",
                          draft_.playback_cache_mebibytes, 256U, 32'768U,
                          "MiB");
  integer_slider_row("Trail", "preferences.playback.trail",
                     draft_.playback_trail_frames, 0U, 16U, "frames");

  // Kept whether or not there is a message, so one arriving does not change
  // how far the form scrolls.
  const gui::Rect message = form.caption();
  if (!state.message.empty()) {
    ctx.label(message, state.message, ctx.theme().text);
  }
  scroll_.end(ctx, form.extent());

  // Retyping a set password is a change even though "set" is unchanged.
  const bool changed = draft_ != state.stored ||
                       (password_edited_ && !password_draft_.empty());
  gui::Rect buttons = modal.footer;
  const gui::Rect save_button = gui::cut_right(buttons, m.button_width);
  static_cast<void>(gui::cut_right(buttons, m.gap));
  const gui::Rect cancel_button = gui::cut_right(buttons, m.button_width);
  const bool cancelled = preferences_button(
      ctx, gui::ui_id(id_, "preferences.cancel"), cancel_button, "CANCEL", true);
  const bool saved = preferences_button(ctx, gui::ui_id(id_, "preferences.save"),
                                        save_button, "SAVE", changed);
  ctx.tooltip(save_button,
              "Saved on this machine only, never in a project. Most settings "
              "apply the next time this program starts.");
  const bool dismissed =
      end_modal(ctx, gui::ui_id(id_, "preferences.scrim"), bounds, modal);

  if (saved) {
    result.save = draft_;
    if (password_edited_) {
      result.render_node_password = password_draft_;
    }
  }
  if (saved || cancelled || dismissed) {
    open_ = false;
    password_draft_.clear();
    password_edited_ = false;
  }
  return result;
}

} // namespace mediaserver::editor
