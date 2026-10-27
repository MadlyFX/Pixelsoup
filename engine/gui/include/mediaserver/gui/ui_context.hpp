#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>

// The immediate-mode widget layer of the bespoke UI. Widgets read the frame's
// InputState, emit into a UiDrawList, and return their interaction result in one
// call, holding no per-widget objects. The state that outlives a frame is which
// widget the pointer is over (hot), which one captured it (active), and which
// one has the keyboard (focus) - so a press-then-release drag tracks one control
// even as the pointer leaves it, and a text field keeps receiving keystrokes.
//
// Identity is an explicit UiId the caller supplies, stable across frames -
// hash a name with ui_id(), or use a fixed integer. Two live widgets sharing an
// id will fight over hot/active, exactly as in any immediate-mode UI.
//
// This layer is deliberately independent of layout: every widget takes an
// explicit pixel Rect. The LayoutTree in layout.hpp sits above and produces
// those rects; the interaction logic here does not change when it does, which is
// why the two are built and tested apart.
namespace mediaserver::gui {

using UiId = core::u64;

// FNV-1a over the name, so callers can identify a widget by a stable string
// instead of managing integers.
[[nodiscard]] constexpr UiId ui_id(std::string_view name) noexcept {
    core::u64 hash = 1469598103934665603ULL;
    for (const char c : name) {
        hash ^= static_cast<core::u8>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// The colours and paddings the widgets paint with: an After Effects-style dark
// theme by default. A caller can swap it wholesale.
//
// The hues run cool and desaturated so that what is actually on the wall - the
// rendered viewport, and a clip's own colour on a timeline bar - is the only
// saturated thing on screen. Everything else is a value, not a colour, which is
// what keeps a dark room legible at a glance.
struct Theme final {
    // The ground behind the panels, and the panels themselves.
    UiColor background{0.043F, 0.047F, 0.059F, 1.0F};
    UiColor panel{0.071F, 0.078F, 0.098F, 1.0F};
    // A section header inside a panel, and the two tints a long list alternates
    // between so similar rows stay countable.
    UiColor header{0.106F, 0.118F, 0.145F, 1.0F};
    UiColor row{0.086F, 0.094F, 0.118F, 1.0F};
    UiColor row_alt{0.071F, 0.078F, 0.098F, 1.0F};

    UiColor widget{0.145F, 0.157F, 0.192F, 1.0F};
    UiColor widget_hover{0.196F, 0.212F, 0.255F, 1.0F};
    UiColor widget_active{0.153F, 0.353F, 0.588F, 1.0F};
    UiColor accent{0.231F, 0.510F, 0.965F, 1.0F};
    // A selected row: the accent held well back, so a whole list of them does
    // not glow.
    UiColor selection{0.114F, 0.208F, 0.361F, 1.0F};
    UiColor track{0.043F, 0.047F, 0.059F, 1.0F};
    UiColor text{0.878F, 0.894F, 0.925F, 1.0F};
    UiColor text_muted{0.443F, 0.475F, 0.545F, 1.0F};
    // A rule inside a panel: between a toolbar and a list, a header and its
    // rows. Visible against both, but quieter than anything with content.
    UiColor divider{0.027F, 0.031F, 0.039F, 1.0F};
    // The edge of a panel, a field, or a floating frame - the line that says
    // where one surface stops. Lighter than the surfaces, so two dark panels
    // side by side never merge into one.
    UiColor border{0.165F, 0.184F, 0.224F, 1.0F};

    // The transport's own colour. Amber because it is the one thing on the
    // timeline that must be found without looking for it, and nothing else in
    // the palette is warm.
    UiColor playhead{0.973F, 0.706F, 0.235F, 1.0F};
    UiColor keyframe{0.471F, 0.784F, 1.0F, 1.0F};
    // A live output, and something that needs an answer.
    UiColor good{0.298F, 0.831F, 0.514F, 1.0F};
    UiColor warning{0.937F, 0.427F, 0.353F, 1.0F};

    float padding{6.0F};
};

// The pointer's shape. A widget asks for one while the pointer is over it, or
// while it holds the pointer, and the window shows the frame's last request. A
// resize arrow is how an operator finds out a seam can be dragged at all.
enum class Cursor : core::u8 {
    Arrow,
    Hand,
    Text,
    ResizeHorizontal,
    ResizeVertical,
    ResizeDiagonal,
    Move,
    RippleTrim,
    Roll,
    Slip,
    Slide,
    Razor,
    Count,
};

// The raw result of pointing at something: what every widget below is built
// from. It is public so that a compound surface - a dock tab bar, a timeline
// clip, anything with its own drawing - can be built on the same hot/active
// rules instead of reimplementing them and drifting from them.
//
// `held` stays true on the frame the release lands, so a widget that tracks the
// pointer gets one last update at the position it was let go at.
struct Interaction final {
    bool hovered{};
    // The press that captured the pointer happened this frame.
    bool pressed{};
    // This widget owns the capture, including the frame it is released.
    bool held{};
    bool released{};
    // Released while still over the widget - a click, rather than a press that
    // slid off and was abandoned.
    bool clicked{};
};

// The two marks a layer list is read by. Every compositing tool draws them,
// nobody writes the letter M, and an operator glancing at a stack from a metre
// away has to see state rather than read it. They live here rather than in one
// panel because the Timeline and the Inspector have to draw the same eye.
//
// `open` means visible and `closed` means locked - the engaged state in each
// case is what the caller colours differently.
void draw_eye_glyph(
    UiDrawList& list, float center_x, float center_y, bool open, UiColor color);
void draw_padlock_glyph(
    UiDrawList& list,
    float center_x,
    float center_y,
    bool closed,
    UiColor color);
// A diagonal pipette silhouette for colour-sampling actions. Kept with the
// shared eye and padlock marks so every colour control can use the same icon.
void draw_eyedropper_glyph(
    UiDrawList& list, float center_x, float center_y, UiColor color);

// The marks every media tool's transport and edit tools are read by. Drawn
// rather than spelled, for the same reason as the eye and padlock: "|<" and
// "RIP" make the operator read, while a play triangle and a razor are found by
// shape. Each fits a 14px square centred on (center_x, center_y).
enum class Icon : core::u8 {
    Play,
    Pause,
    StepBack,
    StepForward,
    ToStart,
    ToEnd,
    Loop,
    // A horseshoe magnet: snapping.
    Snap,
    // A pennant on a staff: a marker.
    Marker,
    // A stop-sign octagon: a Hold cue.
    Hold,
    // Edit tools.
    Select,
    Ripple,
    Roll,
    Slip,
    Slide,
    Razor,
    // Zoom to fit: two inward brackets.
    Fit,
    Plus,
    // Output lifecycle: a filled square stops, a ring with an arrowhead
    // restarts (or retries).
    Stop,
    Restart,
    // Disclosure for a details panel: three bars.
    Details,
    // Two overlapping frames: make a copy.
    Duplicate,
    // A playhead over a part-filled time bar: what playback has cached.
    Cache,
};
void draw_icon(
    UiDrawList& list,
    Icon icon,
    float center_x,
    float center_y,
    UiColor color,
    float scale = 1.0F);

class UiContext final {
public:
    UiContext(
        InputState& input,
        UiDrawList& list,
        const FontAtlas& font,
        Theme theme = Theme{}) noexcept;

    // Clears the per-frame hot state and resets the draw list. Call once at the
    // top of a frame, after InputState::new_frame() and after that frame's
    // events are applied. `active` and `focus` persist so a drag and an edit
    // both survive the frame boundary; a capture whose widget was not drawn
    // last frame is released once the button is up.
    //
    // `time_seconds` drives the caret blink only. It defaults to zero, which
    // holds the caret solid - what a test wants, since a blinking caret would
    // make a draw list depend on the clock.
    void new_frame(double time_seconds = 0.0);

    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] UiDrawList& draw_list() noexcept { return list_; }
    // Monotonic frame time supplied to new_frame(). Compound interactions such
    // as a long-press use the same clock as caret animation, which keeps them
    // deterministic in headless tests and independent of the window backend.
    [[nodiscard]] double time_seconds() const noexcept { return time_; }

    // The width `text` would take in the current font, so a compound surface
    // can size something to its label without holding the atlas itself.
    [[nodiscard]] float font_width(std::string_view text) const {
        return font_.measure(text);
    }
    [[nodiscard]] float line_height() const noexcept { return font_.line_height(); }
    // Text for a zooming surface. `size` is a multiple of the UI font; the
    // answer is the baked size that text is actually drawn and measured at,
    // or nullopt when nothing small enough was baked - skip that text.
    [[nodiscard]] std::optional<float> text_size_at_most(float size) const noexcept {
        return font_.size_at_most(size);
    }
    [[nodiscard]] float font_width(std::string_view text, float size) const {
        return font_.measure(text, size);
    }
    [[nodiscard]] float line_height(float size) const noexcept {
        return font_.line_height(size);
    }

    // Confines both drawing and pointer hit-testing to a layout rectangle.
    // Nested clips intersect, matching the draw list's clipping behavior.
    void push_clip(const Rect& bounds);
    void pop_clip();

    // --- Containers and static content ---

    // A solid rectangle in the theme's panel colour (or a given colour). Panels
    // do not clip on their own; push a clip rect for that.
    void panel(const Rect& bounds);
    void panel(const Rect& bounds, UiColor color);

    // Left-aligned, vertically centred text inside `bounds`.
    void label(const Rect& bounds, std::string_view text);
    void label(const Rect& bounds, std::string_view text, UiColor color);
    // The same, at a baked text size from text_size_at_most().
    void label(const Rect& bounds, std::string_view text, UiColor color,
               float size);
    // Hover details retain long paths and diagnostics without occupying rows.
    void tooltip(const Rect& bounds, std::string_view text);

    // An engine texture drawn into `bounds` - a rendered viewport composited
    // into the widget tree with no copy, which is the whole reason the UI
    // shares the engine's RHI. `tint` multiplies it; white shows it as rendered.
    void image(const Rect& bounds, const rhi::Texture& texture);
    void image(const Rect& bounds, const rhi::Texture& texture, UiColor tint);

    // The same, letterboxed inside `bounds` at `aspect` (width over height) and
    // centred, so a 16:9 render never stretches into a panel that is not. The
    // rectangle it actually filled is returned, because a viewport has to map
    // pointer positions back through exactly that placement.
    Rect image_fit(const Rect& bounds, const rhi::Texture& texture, float aspect);

    // --- Interactive widgets ---

    // Runs the hot/active bookkeeping for `bounds` and reports what happened,
    // drawing nothing. The primitive the widgets below and the dock host are
    // built on.
    [[nodiscard]] Interaction grab(UiId id, const Rect& bounds) noexcept;

    // Temporarily masks pointer input for a visual layer underneath an
    // occluder. Drawing is unaffected. Nested blocks are supported so a host
    // can walk several stacked floating panels back-to-front, blocking every
    // covered callback until it reaches the frontmost one.
    void push_pointer_block() noexcept;
    void pop_pointer_block() noexcept;
    [[nodiscard]] bool pointer_blocked() const noexcept {
        return pointer_block_depth_ != 0U || dropdown_id_ != 0U || dropdown_dismissed_;
    }

    // A push button. Returns true on the frame the press is released over it.
    [[nodiscard]] bool button(UiId id, const Rect& bounds, std::string_view label);

    // A horizontal slider over [min, max]. Writes `value` and returns true on a
    // frame it changed.
    [[nodiscard]] bool slider(
        UiId id, const Rect& bounds, float& value, float min, float max);

    // A drag field: horizontal drag changes `value` by pixels times `speed`,
    // with no bounds. The inspector's numeric workhorse. Returns true when it
    // changed.
    [[nodiscard]] bool drag_float(
        UiId id, const Rect& bounds, float& value, float speed);

    // A labelled checkbox. Toggles `value`; returns true on the frame it toggled.
    [[nodiscard]] bool checkbox(
        UiId id, const Rect& bounds, bool& value, std::string_view label);

    // Menus own input until selection, Escape, or an outside click. The host
    // draws their overlay once, after all panels, so it escapes panel clips.
    [[nodiscard]] bool dropdown(UiId id, const Rect& bounds,
                                std::span<const std::string_view> options,
                                std::size_t& selected);

    // A right-click menu, the same overlay a dropdown opens but anchored at a
    // point rather than under a field. `open_context_menu` opens it - usually
    // on `secondary_pointer_pressed()` - and the owner then calls
    // `context_menu` every frame with that id and its current options; an
    // owner that stops calling is gone, and the menu closes with it. Returns
    // the chosen option, once, on the frame the choice lands. Like any menu it
    // owns the pointer and keyboard while open, so nothing underneath reacts.
    void open_context_menu(UiId id, float x, float y);
    [[nodiscard]] std::optional<std::size_t> context_menu(
        UiId id, std::span<const std::string_view> options);
    // The same menu with a colour chip before each entry, for a choice that
    // is a colour. `swatches` pairs with `options` by index; an entry whose
    // swatch is fully transparent draws an empty chip ("None").
    [[nodiscard]] std::optional<std::size_t> context_menu(
        UiId id, std::span<const std::string_view> options,
        std::span<const UiColor> swatches);
    [[nodiscard]] bool menu_open(UiId id) const noexcept {
        return dropdown_id_ == id;
    }
    void draw_overlays(const Rect& window_bounds);

    // A single-line editable field. Clicking it takes the keyboard and puts the
    // caret at the clicked character; typing, Backspace/Delete, and the arrow,
    // Home and End keys edit from there. Enter and Tab commit, Escape restores
    // what the field held when the edit began, and a press anywhere else
    // commits. Returns true on a frame `value` changed.
    //
    // The caret moves over UTF-8 boundaries rather than bytes, so a multi-byte
    // character is never left half-deleted, though the ASCII atlas will not
    // draw one yet.
    [[nodiscard]] bool text_field(UiId id, const Rect& bounds, std::string& value);
    // A text field that shows one '*' per character instead of the text: a
    // password. Editing, focus and commit behave exactly as text_field.
    [[nodiscard]] bool password_field(UiId id, const Rect& bounds,
                                      std::string& value);

    // Puts the keyboard in `id`'s field with the caret at the end, as though it
    // had been clicked. A compound widget decides for itself when typing begins
    // - an inspector field scrubs on a drag and types on a double-click - and by
    // then the click that would have focused it has already been spent on the
    // drag logic, so it has to say so instead.
    void focus_text_field(UiId id, const std::string& value);
    // The same, with the whole value selected: the first character typed
    // replaces it, Backspace clears it, and an arrow key or a click drops the
    // selection where it lands. How a rename opens.
    void focus_text_field_selected(UiId id, const std::string& value);
    // Drops the keyboard, committing whatever a focused field holds. What a
    // compound widget calls when its own edit ends.
    void clear_focus() noexcept;

    // A draggable divider across `bounds`, at `fraction` (0..1) along `axis` -
    // Row puts the panes side by side, Column stacks them. Dragging writes
    // `fraction`, clamped so neither pane can be squeezed away. The returned
    // rectangles already reflect this frame's drag.
    Split split(
        UiId id,
        const Rect& bounds,
        float& fraction,
        Axis axis,
        float thickness = 6.0F);

    // The pointer, for a compound surface that needs it beyond what grab()
    // reports - dragging a dock tab across the window, say, where the question
    // is where the pointer is now rather than what it is over.
    [[nodiscard]] float pointer_x() const noexcept { return input_.mouse_x(); }
    [[nodiscard]] float pointer_y() const noexcept { return input_.mouse_y(); }
    // Where this frame's left press began. A native event batch can include
    // motion and release after that press, so drag anchors use these instead
    // of the latest pointer position above.
    [[nodiscard]] float pointer_press_x() const noexcept {
        return input_.mouse_press_x(MouseButton::Left);
    }
    [[nodiscard]] float pointer_press_y() const noexcept {
        return input_.mouse_press_y(MouseButton::Left);
    }
    // The first release endpoint, before any later unheld motion in the batch.
    [[nodiscard]] float pointer_release_x() const noexcept {
        return input_.mouse_release_x(MouseButton::Left);
    }
    [[nodiscard]] float pointer_release_y() const noexcept {
        return input_.mouse_release_y(MouseButton::Left);
    }
    [[nodiscard]] float pointer_dx() const noexcept {
        return pointer_blocked() ? 0.0F : input_.mouse_dx();
    }
    [[nodiscard]] float pointer_dy() const noexcept {
        return pointer_blocked() ? 0.0F : input_.mouse_dy();
    }
    [[nodiscard]] bool pointer_down() const noexcept {
        return !pointer_blocked() && input_.mouse_down(MouseButton::Left);
    }
    [[nodiscard]] bool pointer_pressed() const noexcept {
        return !pointer_blocked() && input_.mouse_pressed(MouseButton::Left);
    }
    [[nodiscard]] bool pointer_released() const noexcept {
        return !pointer_blocked() && input_.mouse_released(MouseButton::Left);
    }
    [[nodiscard]] bool middle_pointer_down() const noexcept {
        return !pointer_blocked() && input_.mouse_down(MouseButton::Middle);
    }
    [[nodiscard]] bool middle_pointer_pressed() const noexcept {
        return !pointer_blocked() && input_.mouse_pressed(MouseButton::Middle);
    }
    [[nodiscard]] bool middle_pointer_released() const noexcept {
        return !pointer_blocked() && input_.mouse_released(MouseButton::Middle);
    }
    // The right button: a context menu, or a 3D viewport's camera look.
    [[nodiscard]] bool secondary_pointer_pressed() const noexcept {
        return !pointer_blocked() && input_.mouse_pressed(MouseButton::Right);
    }
    [[nodiscard]] bool secondary_pointer_down() const noexcept {
        return !pointer_blocked() && input_.mouse_down(MouseButton::Right);
    }
    // Raw mouse travel this frame. Unlike pointer_dx() it keeps arriving while
    // the pointer is locked for a camera drag.
    [[nodiscard]] float pointer_motion_x() const noexcept {
        return pointer_blocked() ? 0.0F : input_.motion_dx();
    }
    [[nodiscard]] float pointer_motion_y() const noexcept {
        return pointer_blocked() ? 0.0F : input_.motion_dy();
    }
    // A movement key held down. Never while a field or menu has the keyboard.
    [[nodiscard]] bool move_key_down(MoveKey key) const noexcept {
        return focus_ == UiId{} && dropdown_id_ == 0U && input_.move_key_down(key);
    }
    // This frame's wheel travel, for a surface that zooms or scrolls - the
    // timeline's time axis, a viewport's dolly.
    [[nodiscard]] float wheel() const noexcept {
        return pointer_blocked() ? 0.0F : input_.wheel();
    }
    // Files dragged in from the desktop: whether a drag is over the window
    // and where, and the paths let go this frame with where they landed.
    [[nodiscard]] bool file_drag_over() const noexcept {
        return input_.file_drag_over();
    }
    [[nodiscard]] float file_drag_x() const noexcept {
        return input_.file_drag_x();
    }
    [[nodiscard]] float file_drag_y() const noexcept {
        return input_.file_drag_y();
    }
    [[nodiscard]] std::span<const std::string> dropped_files() const noexcept {
        return input_.dropped_files();
    }
    [[nodiscard]] float drop_x() const noexcept { return input_.drop_x(); }
    [[nodiscard]] float drop_y() const noexcept { return input_.drop_y(); }
    [[nodiscard]] bool shift_held() const noexcept {
        return has_mod(input_.mods(), KeyMods::Shift);
    }
    [[nodiscard]] bool ctrl_held() const noexcept {
        return has_mod(input_.mods(), KeyMods::Ctrl);
    }
    [[nodiscard]] bool alt_held() const noexcept {
        return has_mod(input_.mods(), KeyMods::Alt);
    }
    // A non-text key struck this frame. Shortcuts read this; a field with the
    // keyboard has already consumed what it needed by the time they do, which
    // is what `typing()` below is for.
    [[nodiscard]] bool key_pressed(Key key) const noexcept;
    // True while some field owns the keyboard, so a bare Delete or Ctrl+Z
    // shortcut stands down rather than firing mid-word - and while a viewport
    // camera is being flown, whose W, A, S, D, Q and E are movement.
    [[nodiscard]] bool typing() const noexcept {
        return focus_ != UiId{} || dropdown_id_ != 0U || dropdown_dismissed_ ||
               navigating_;
    }
    // A viewport has the keyboard - camera movement keys, or a keyboard
    // transform's axis letters and typed value - until this is cleared.
    void set_navigating(bool navigating) noexcept { navigating_ = navigating; }
    // Printable characters struck this frame while nothing is typing - the
    // single-key shortcuts a timeline lives on (Space, I, O, M). Empty while a
    // field or menu owns the keyboard, so a name being typed never also plays
    // the show.
    [[nodiscard]] std::string_view shortcut_text() const noexcept {
        return typing() ? std::string_view{} : input_.text();
    }
    // This frame's printable characters whoever has the keyboard. Only for
    // the owner of a set_navigating() claim - a viewport running a keyboard
    // transform reads its axis letters and typed value here while every
    // other shortcut stands down.
    [[nodiscard]] std::string_view claimed_text() const noexcept {
        return focus_ == UiId{} && dropdown_id_ == 0U ? input_.text()
                                                      : std::string_view{};
    }

    // Asks for a pointer shape this frame. Later requests win, so the topmost
    // thing drawn under the pointer decides; new_frame() resets it to Arrow.
    void set_cursor(Cursor cursor) noexcept { cursor_ = cursor; }
    [[nodiscard]] Cursor cursor() const noexcept { return cursor_; }
    // set_cursor() for a widget's own interaction: while it is hovered and free
    // to take the pointer, or while it holds it.
    void cursor_for(const Interaction& interaction, Cursor cursor) noexcept {
        if (interaction.held || (interaction.hovered && !pointer_blocked())) {
            cursor_ = cursor;
        }
    }

    [[nodiscard]] UiId hot() const noexcept { return hot_; }
    [[nodiscard]] UiId active() const noexcept { return active_; }
    [[nodiscard]] UiId focus() const noexcept { return focus_; }
    // Where the caret sits in the focused field, as a byte offset. Exposed for
    // tests and for a caller that wants to place it itself.
    [[nodiscard]] std::size_t caret() const noexcept { return caret_; }

private:
    // The shared hot/active bookkeeping every interactive widget runs: sets hot
    // when hovered and free, and captures active on press. Returns whether this
    // widget is hovered (for the caller's own release logic) via `hovered_out`.
    // `pressed_out` reports the press that started a capture this frame.
    void interact(
        UiId id,
        const Rect& bounds,
        bool& hovered_out,
        bool& pressed_out) noexcept;

    // Gives `id` the keyboard, remembering what the field held so Escape can
    // put it back, and restarts the blink so the caret shows immediately.
    void take_focus(UiId id, const std::string& value);
    // Applies this frame's keystrokes to a focused field. Returns whether the
    // text changed; `closed_out` reports a commit or cancel.
    bool edit_keys(std::string& value, bool& closed_out);
    bool edit_field(UiId id, const Rect& bounds, std::string& value,
                    bool masked);

    InputState& input_;
    UiDrawList& list_;
    const FontAtlas& font_;
    Theme theme_;
    UiId hot_{};
    UiId active_{};
    // The capturing widget ran this frame; see new_frame().
    bool active_seen_{};
    bool press_consumed_{};
    UiId focus_{};
    std::size_t caret_{};
    // The focused field's whole value is selected (see
    // focus_text_field_selected).
    bool select_all_{};
    bool navigating_{};
    // What the focused field held when the edit began, for Escape.
    std::string edit_original_;
    double time_{};
    // The instant the caret was last forced visible - a focus change or a
    // keystroke - so it does not blink out from under the operator mid-type.
    double blink_base_{};
    core::u32 pointer_block_depth_{};
    std::vector<Rect> interaction_clip_stack_;
    UiId dropdown_id_{};
    UiId dropdown_result_id_{};
    std::size_t dropdown_result_{};
    std::size_t dropdown_highlight_{};
    std::size_t dropdown_first_{};
    std::size_t dropdown_pressed_{static_cast<std::size_t>(-1)};
    std::vector<std::string> dropdown_options_;
    std::vector<UiColor> dropdown_swatches_;
    Rect dropdown_anchor_{};
    Rect dropdown_bounds_{};
    bool dropdown_dismissed_{};
    bool dropdown_seen_{};
    std::string tooltip_text_;
    std::string tooltip_previous_;
    double tooltip_since_{};
    Cursor cursor_{Cursor::Arrow};
};

} // namespace mediaserver::gui
