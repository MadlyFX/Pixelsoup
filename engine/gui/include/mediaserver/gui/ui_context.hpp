#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <cstddef>
#include <string>
#include <string_view>

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
    // The seam between docked panels - darker than either, so the split reads
    // as a gap rather than a border.
    UiColor divider{0.027F, 0.031F, 0.039F, 1.0F};

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
    // both survive the frame boundary.
    //
    // `time_seconds` drives the caret blink only. It defaults to zero, which
    // holds the caret solid - what a test wants, since a blinking caret would
    // make a draw list depend on the clock.
    void new_frame(double time_seconds = 0.0);

    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] UiDrawList& draw_list() noexcept { return list_; }

    // The width `text` would take in the current font, so a compound surface
    // can size something to its label without holding the atlas itself.
    [[nodiscard]] float font_width(std::string_view text) const {
        return font_.measure(text);
    }
    [[nodiscard]] float line_height() const noexcept { return font_.line_height(); }

    // Clipping passthroughs, so a caller with a Rect from the layout tree can
    // confine a panel's contents without reaching for the draw list.
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

    // Puts the keyboard in `id`'s field with the caret at the end, as though it
    // had been clicked. A compound widget decides for itself when typing begins
    // - an inspector field scrubs on a drag and types on a double-click - and by
    // then the click that would have focused it has already been spent on the
    // drag logic, so it has to say so instead.
    void focus_text_field(UiId id, const std::string& value);
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
    [[nodiscard]] float pointer_dx() const noexcept { return input_.mouse_dx(); }
    [[nodiscard]] float pointer_dy() const noexcept { return input_.mouse_dy(); }
    [[nodiscard]] bool pointer_down() const noexcept {
        return input_.mouse_down(MouseButton::Left);
    }
    // This frame's wheel travel, for a surface that zooms or scrolls - the
    // timeline's time axis, a viewport's dolly.
    [[nodiscard]] float wheel() const noexcept { return input_.wheel(); }
    [[nodiscard]] bool shift_held() const noexcept {
        return has_mod(input_.mods(), KeyMods::Shift);
    }
    [[nodiscard]] bool ctrl_held() const noexcept {
        return has_mod(input_.mods(), KeyMods::Ctrl);
    }
    // A non-text key struck this frame. Shortcuts read this; a field with the
    // keyboard has already consumed what it needed by the time they do, which
    // is what `typing()` below is for.
    [[nodiscard]] bool key_pressed(Key key) const noexcept;
    // True while some field owns the keyboard, so a bare Delete or Ctrl+Z
    // shortcut stands down rather than firing mid-word.
    [[nodiscard]] bool typing() const noexcept { return focus_ != UiId{}; }

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

    InputState& input_;
    UiDrawList& list_;
    const FontAtlas& font_;
    Theme theme_;
    UiId hot_{};
    UiId active_{};
    UiId focus_{};
    std::size_t caret_{};
    // What the focused field held when the edit began, for Escape.
    std::string edit_original_;
    double time_{};
    // The instant the caret was last forced visible - a focus change or a
    // keystroke - so it does not blink out from under the operator mid-type.
    double blink_base_{};
};

} // namespace mediaserver::gui
