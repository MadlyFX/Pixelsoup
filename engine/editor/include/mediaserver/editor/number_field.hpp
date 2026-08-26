#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <string>
#include <string_view>

// The editor's numeric field: drag it sideways to scrub, click it to type.
//
// It is the control an operator spends the most time in, and the one place the
// document's latency shows through. A scrub holds the value it began at and
// adds the pointer's *total* travel to it, rather than accumulating against the
// value it was handed each frame: the committed document is several frames
// behind a live drag, so re-basing every frame makes the field crawl at a
// fraction of the pointer's speed - which reads as "the mouse feels heavy"
// rather than as a fault.
//
// One instance holds the state for a whole panel's worth of fields, because
// only one of them can be live: the widget layer gives the pointer and the
// keyboard to exactly one id at a time.
namespace mediaserver::editor {

class NumberField final {
public:
    // Draws the field for `id`. `axis` is the letter shown at its left (empty
    // for a bare number); `speed` is how much one pixel of drag is worth.
    // Returns true and writes `out` on a frame the value changed.
    [[nodiscard]] bool draw(
        gui::UiContext& ctx,
        gui::UiId id,
        const gui::Rect& bounds,
        std::string_view axis,
        double value,
        double speed,
        double& out,
        int decimals = 3);

    // True while some field is mid-scrub or mid-type. The shell holds one
    // gesture open for as long as it is, so a drag across a field is one undo
    // step; and a bare Delete shortcut stands down.
    [[nodiscard]] bool live() const noexcept {
        return scrub_ != gui::UiId{} || typing_ != gui::UiId{};
    }

    // Gives the keyboard back and abandons any scrub - what a panel calls when
    // the thing being edited has gone away underneath it.
    void cancel(gui::UiContext& ctx);

private:
    // The field being scrubbed, the value it held when the drag began, and the
    // pointer travel since. `moved` separates a scrub from a click that never
    // went anywhere, which is what opens the field for typing instead.
    gui::UiId scrub_{};
    double scrub_base_{};
    float scrub_travel_{};
    bool scrub_moved_{};

    // The field being typed into, and its text.
    gui::UiId typing_{};
    std::string typed_;
};

} // namespace mediaserver::editor
