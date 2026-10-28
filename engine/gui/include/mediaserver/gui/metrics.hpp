#pragma once

// The sizes panels are laid out with, the way Theme holds the colours they are
// painted with. A panel that asks for `metrics.control` instead of writing 24
// gets the same row height as every other panel, and a later density or UI
// scale change is one edit here rather than a hunt through every file.
//
// These are the editor's rhythm, not a widget's internals: how tall a field
// row is and how far apart rows sit, not how a checkbox draws its tick.
namespace mediaserver::gui {

struct Metrics final {
    // A field, button, checkbox or slider row.
    float control{24.0F};
    // Between controls stacked one above the next.
    float row_gap{6.0F};
    // Between controls side by side, and the text padding inside a field.
    float gap{6.0F};
    // From a dialog card's edge to its content.
    float inset{14.0F};
    // From a docked panel's edge to its content: narrower than a card's,
    // because a dock is narrow to begin with.
    float panel_inset{10.0F};
    // The label column of a form, left of its fields.
    float label_column{132.0F};
    // A line of secondary text, or a section heading.
    float caption{18.0F};
    // Below a caption line, before whatever follows it.
    float caption_gap{2.0F};
    // Either side of the rule that opens a form section.
    float section_gap{10.0F};
    // A dialog's or panel's title bar.
    float header{34.0F};
    // A rule between regions.
    float divider{1.0F};
    // A button that confirms, cancels or acts beside a field.
    float button_width{84.0F};
    // What one wheel notch scrolls a list or form.
    float scroll_step{36.0F};
};

} // namespace mediaserver::gui
