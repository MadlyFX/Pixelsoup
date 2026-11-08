#pragma once

// The Preferences dialog: machine-local settings, never replicated.

#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <optional>
#include <string>

namespace mediaserver::editor {

// Machine-local settings the operator would otherwise pass on the command
// line. These are not document state: nothing here is saved into a project or
// replicated, so editing them authors no command.
//
// Every field takes effect when the process starts - except the render node
// password, which applies at once. The panel says which is which rather than
// implying a live change, and a command-line argument given this run still
// wins, so a scripted launch stays exactly reproducible.
struct EditorPreferences final {
  enum class TranscodePriority : core::u8 {
    Background,
    Balanced,
    Fast,
  };

  std::string director_listen;
  std::string show_id;
  std::string director_epoch;
  // Whether a render node password is set. The password itself never comes
  // back to the panel: it is kept only as the key it derives.
  bool render_node_password{};
  std::string wal_path;
  std::string proxy_cache_directory;
  bool proxy_cache_next_to_source{};
  core::u32 proxy_cache_gibibytes{8U};
  core::u32 output_buffer_frames{12U};
  core::u32 output_read_ahead_frames{12U};
  core::u32 playback_cache_mebibytes{2'048U};
  core::u32 playback_trail_frames{};
  TranscodePriority proxy_transcode_priority{TranscodePriority::Fast};

  bool operator==(const EditorPreferences &) const = default;
};

// What the panel shows: the stored values it edits, what this run is actually
// using, and whether a command-line argument overrode each field.
struct EditorPreferencesState final {
  EditorPreferences stored;
  EditorPreferences active;
  bool director_listen_overridden{};
  bool show_id_overridden{};
  bool director_epoch_overridden{};
  // --lifecycle-password or --lifecycle-secret-file was given this run, so
  // the stored password is not the one in use until the next plain launch.
  bool render_node_password_overridden{};
  bool wal_path_overridden{};
  // Result of the last save attempt, shown in the panel. Empty means nothing
  // has been attempted this session.
  std::string message;
};

struct PreferencesFrame final {
  // Present only on the frame the operator pressed Save.
  std::optional<EditorPreferences> save;
  // With `save`, when the operator typed a new render node password or
  // cleared it: the new password, empty for none. Absent keeps the stored one.
  std::optional<std::string> render_node_password;
  // Clears the cache currently in use on this machine. This is intentionally
  // independent of Save so an unsaved draft can never redirect a deletion.
  bool clear_proxy_cache{};
};

// The Preferences dialog: modal, editor-local, and authoring nothing.
//
// It owns its draft so cancelling really discards, and it dims and covers the
// whole window. The scrim is hit-tested after every control in the card,
// because gui::UiContext resolves a frame's pointer first-wins: a full-screen
// grab registered first would swallow the clicks meant for the card. Callers
// must also deny the pointer to everything drawn earlier in the frame - see
// gui::UiContext::push_pointer_block - or the workspace behind stays live.
class PreferencesPanel final {
public:
  explicit PreferencesPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const EditorPreferences &stored);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] const EditorPreferences &draft() const noexcept {
    return draft_;
  }

  // `bounds` is the whole window. Returns a save request when the operator
  // pressed Save; closing is reported through is_open().
  [[nodiscard]] PreferencesFrame draw(gui::UiContext &ctx,
                                      const gui::Rect &bounds,
                                      const EditorPreferencesState &state);

private:
  gui::UiId id_{};
  bool open_{};
  gui::ScrollRegion scroll_;
  EditorPreferences draft_;
  // The password being typed, and whether the operator touched it at all.
  std::string password_draft_;
  bool password_edited_{};
};

} // namespace mediaserver::editor
