#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The pieces every operator list is built from.
//
// The Scene outliner and the Timeline's layer list show the same elements, so
// they draw them with the same marks: one eye, one padlock, one icon per kind
// of element, one name per element, and one way to rename it. Keeping those
// here rather than in either panel is what stops the two drifting apart - a
// checkbox labelled "Lock" in one list and a padlock in the other is exactly
// the inconsistency this file exists to prevent.
namespace mediaserver::editor {

// Shared list geometry, in logical units. Rows are tall enough to hit with a
// pointer from arm's length without aiming; toggles are square targets.
namespace list_metrics {
inline constexpr float row_height = 28.0F;
inline constexpr float indent = 16.0F;
inline constexpr float toggle = 24.0F;
inline constexpr float icon = 16.0F;
// Where a row's twirl sits relative to its indent, and where the icon and
// the name start after it.
inline constexpr float twirl_width = 16.0F;
} // namespace list_metrics

// The one name an element goes by in every list, header and tooltip: its
// authored name, or its kind and node number when it has none ("Point Light
// 1204"). Never a path or an internal identifier.
[[nodiscard]] std::string element_display_name(const ElementInfo &element);

// Neutral marks; icon shape identifies the kind without an arbitrary palette.
[[nodiscard]] gui::UiColor element_kind_color(const ElementInfo &element) noexcept;
[[nodiscard]] gui::UiColor element_kind_color(ElementKind kind) noexcept;

// A 16-unit icon for the element's kind, centred on (cx, cy).
void draw_element_icon(gui::UiContext &ctx, const ElementInfo &element,
                       float cx, float cy, gui::UiColor color);
void draw_kind_icon(gui::UiContext &ctx, ElementKind kind,
                    scene::LocalLightKind light, float cx, float cy,
                    gui::UiColor color);

// The two switches every listed element has. `state` is the element's own
// value: visible for Visible, locked for Locked.
enum class RowFlag : core::u8 {
  Visible,
  Locked,
};

// An eye or a padlock in a square target. The resting state (visible,
// unlocked) is drawn quietly - shown faintly on every row so the column reads
// as a column - and the state worth noticing (hidden, locked) is drawn bright.
// Returns true on the frame it is clicked.
[[nodiscard]] bool row_flag_toggle(gui::UiContext &ctx, gui::UiId id,
                                   const gui::Rect &bounds, RowFlag flag,
                                   bool state, bool row_hovered);

// The disclosure triangle every tree uses.
void draw_twirl(gui::UiContext &ctx, float cx, float cy, bool open,
                gui::UiColor color);

// Faint vertical rules, one per ancestor, so depth reads at a glance even
// where a row's twirl is closed or absent. `x` is the left of depth zero.
void draw_indent_guides(gui::UiContext &ctx, float x, float y, float height,
                        std::size_t depth, gui::UiColor color);

// A label in a filled lozenge - LIVE, PGM, 3 KEYS - sized to its text.
[[nodiscard]] float pill_width(const gui::UiContext &ctx, std::string_view text);
gui::Rect draw_pill(gui::UiContext &ctx, float x, float centre_y,
                    std::string_view text, gui::UiColor color,
                    bool filled = true);
void draw_status_dot(gui::UiContext &ctx, float cx, float cy,
                     gui::UiColor color, float radius = 3.5F);
void draw_filled_circle(gui::UiContext &ctx, float cx, float cy, float radius,
                        gui::UiColor color);

// A search box in `field`: the text, its placeholder while it is empty and
// not being typed in, and a magnifier at its left in the room the placeholder
// leaves. Returns true on the frame the text changes.
bool search_box(gui::UiContext &ctx, gui::UiId id, const gui::Rect &field,
                std::string &value, std::string_view placeholder);

// A square tool button carrying a drawn icon. Body only on hover, press, or
// when engaged, so a toolbar of them reads as marks rather than a wall of
// boxes. The tooltip names the action.
[[nodiscard]] bool icon_button(gui::UiContext &ctx, gui::UiId id,
                               const gui::Rect &bounds, gui::Icon icon,
                               std::string_view tooltip, bool engaged = false,
                               bool enabled = true);

// A transport or tool key: a soft rounded key with a body at rest, eased
// lighter under the pointer, and lit in `engaged_color` while its mode is on
// - Play while playing, Loop, the current edit tool. The Timeline's and the
// Asset Viewer's transports are both built of these, so they read the same.
[[nodiscard]] bool tool_key(gui::UiContext &ctx, gui::UiId id,
                            const gui::Rect &bounds, gui::Icon icon,
                            bool engaged, std::string_view tooltip,
                            gui::UiColor engaged_color,
                            float icon_scale = 1.0F);

// How a pill button reads. Quiet for everyday actions, Primary for the one
// thing a panel most wants pressed (Import), Danger for what cannot be taken
// back lightly - ringed and inked in red, never filled.
enum class ButtonTone : core::u8 {
  Quiet,
  Primary,
  Danger,
};

// A pill carrying a drawn icon, a word, or both, centred: the operator's
// everyday button. An icon alone needs `tooltip` to name it. `engaged` lights
// a toggle that is on. Returns true on the frame it is clicked.
[[nodiscard]] bool pill_button(gui::UiContext &ctx, gui::UiId id,
                               const gui::Rect &bounds,
                               std::optional<gui::Icon> icon,
                               std::string_view label,
                               std::string_view tooltip,
                               ButtonTone tone = ButtonTone::Quiet,
                               bool enabled = true, bool engaged = false);

// The mark a value carries while it differs from its default: a small amber
// arrow curling back, as in Unreal's details panel. Shown only when there is
// something to put back; a click asks for the default. `tooltip` names the
// default, so the operator knows what they are about to get.
[[nodiscard]] bool reset_button(gui::UiContext &ctx, gui::UiId id,
                                const gui::Rect &bounds,
                                std::string_view tooltip);
// The square a reset arrow takes at the end of a label.
inline constexpr float reset_button_width = 18.0F;

// A bordered text button for panel strips and list rows: dimmed and inert
// while disabled, and filled with the selection colour while `selected` - a
// toggle that is on. Returns true on the frame it is clicked.
[[nodiscard]] bool action_button(gui::UiContext &ctx, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool enabled,
                                 bool selected = false);

// A region that will take what is being dragged: tinted and outlined, more
// strongly while `hovered`, with a callout in its top-left corner - at most
// `callout_width` wide - saying what releasing there does. Every valid target
// shows; the one under the pointer becomes materially stronger.
void draw_drop_target(gui::UiContext &ctx, const gui::Rect &area,
                      std::string_view instruction, bool hovered,
                      float callout_width);

// A diamond centred on (cx, cy), `radius` from its centre to each point.
void draw_diamond(gui::UiContext &ctx, float cx, float cy, float radius,
                  gui::UiColor color);

// The key every keyable value carries, centred in `bounds`: hollow with no key
// at this frame, filled with one, in the keyframe colour while the value is
// animated elsewhere. `background` fills the hollow. Returns true on the
// frame it is clicked.
[[nodiscard]] bool key_button(gui::UiContext &ctx, gui::UiId id,
                              const gui::Rect &bounds, bool animated,
                              bool keyed_here, gui::UiColor background);

// Recognises a second click on the same row within the double-click window.
// Rows are compound widgets that see one click a frame, so the timing lives
// here rather than in the input layer.
class ClickTracker final {
public:
  // Records a click on `key` and reports whether it completes a double-click.
  [[nodiscard]] bool double_click(core::u64 key, double now) noexcept;
  void reset() noexcept;

private:
  core::u64 key_{};
  double time_{-1.0};
};

// In-place renaming for a list row: double-click or F2 opens a field over the
// name with the whole name selected, typing replaces it, Enter or a click
// elsewhere commits, Escape abandons. One instance per list; only one row is
// ever being renamed.
class InlineRename final {
public:
  explicit InlineRename(gui::UiId id) noexcept : id_(id) {}

  void begin(gui::UiContext &ctx, core::u64 key, std::string_view current);
  [[nodiscard]] bool editing(core::u64 key) const noexcept {
    return key_ != 0U && key_ == key;
  }
  [[nodiscard]] bool active() const noexcept { return key_ != 0U; }
  [[nodiscard]] core::u64 key() const noexcept { return key_; }

  // Draws the field over `bounds`. Returns the new name once, on the frame a
  // real change is committed; an unchanged, blank, or abandoned edit ends
  // returning nothing.
  [[nodiscard]] std::optional<std::string> draw(gui::UiContext &ctx,
                                                const gui::Rect &bounds);
  // Call once after the list has drawn: a row that scrolled away or vanished
  // takes its field with it rather than leaving the keyboard captured.
  void end_frame(gui::UiContext &ctx) noexcept;
  void cancel(gui::UiContext &ctx) noexcept;
  // Forgets the edit without touching the keyboard: for a project load, where
  // the context's own focus is reset separately.
  void reset() noexcept;

private:
  gui::UiId id_{};
  core::u64 key_{};
  std::string draft_;
  std::string original_;
  bool drawn_{};
};

} // namespace mediaserver::editor
