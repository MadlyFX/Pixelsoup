#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

// The proof of the layout steps lighter than the LayoutTree: rectangle cuts,
// the form cursor, and the scroll region. Cuts and the form are arithmetic, so
// they are asserted as rectangles; the scroll region is driven with synthetic
// wheel input against an empty atlas, as the widget suite does.
//
// Every case uses metrics unlike the defaults, so a primitive that quietly
// read a hard-coded size instead of the Metrics it was given would fail here.
namespace {

namespace gui = mediaserver::gui;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define UI_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b) noexcept {
    return std::fabs(a - b) < 0.01F;
}

[[nodiscard]] bool same_rect(
    const gui::Rect& r, float x, float y, float w, float h) noexcept {
    return near(r.x, x) && near(r.y, y) && near(r.width, w)
        && near(r.height, h);
}

[[nodiscard]] gui::Metrics odd_metrics() {
    gui::Metrics metrics{};
    metrics.control = 20.0F;
    metrics.row_gap = 5.0F;
    metrics.gap = 3.0F;
    metrics.inset = 10.0F;
    metrics.label_column = 100.0F;
    metrics.caption = 15.0F;
    metrics.caption_gap = 1.0F;
    metrics.section_gap = 7.0F;
    metrics.divider = 2.0F;
    metrics.scroll_step = 25.0F;
    return metrics;
}

void edges_and_centre_read_off_the_rect() {
    constexpr gui::Rect r{10.0F, 20.0F, 300.0F, 40.0F};
    UI_CHECK(near(r.right(), 310.0F));
    UI_CHECK(near(r.bottom(), 60.0F));
    UI_CHECK(near(r.center_x(), 160.0F));
    UI_CHECK(near(r.center_y(), 40.0F));
}

void each_cut_takes_its_edge_and_shrinks_the_rest() {
    gui::Rect rest{10.0F, 20.0F, 300.0F, 200.0F};
    const gui::Rect top = gui::cut_top(rest, 30.0F);
    const gui::Rect bottom = gui::cut_bottom(rest, 40.0F);
    const gui::Rect left = gui::cut_left(rest, 50.0F);
    const gui::Rect right = gui::cut_right(rest, 60.0F);

    UI_CHECK(same_rect(top, 10.0F, 20.0F, 300.0F, 30.0F));
    UI_CHECK(same_rect(bottom, 10.0F, 180.0F, 300.0F, 40.0F));
    UI_CHECK(same_rect(left, 10.0F, 50.0F, 50.0F, 130.0F));
    UI_CHECK(same_rect(right, 250.0F, 50.0F, 60.0F, 130.0F));
    UI_CHECK(same_rect(rest, 60.0F, 50.0F, 190.0F, 130.0F));
}

void a_cut_larger_than_the_rect_takes_only_what_is_there() {
    gui::Rect rest{0.0F, 0.0F, 100.0F, 20.0F};
    const gui::Rect top = gui::cut_top(rest, 35.0F);
    UI_CHECK(same_rect(top, 0.0F, 0.0F, 100.0F, 20.0F));
    UI_CHECK(same_rect(rest, 0.0F, 20.0F, 100.0F, 0.0F));

    gui::Rect narrow{0.0F, 0.0F, 30.0F, 20.0F};
    const gui::Rect right = gui::cut_right(narrow, 45.0F);
    UI_CHECK(same_rect(right, 0.0F, 0.0F, 30.0F, 20.0F));
    UI_CHECK(near(narrow.width, 0.0F));

    // A negative request takes nothing rather than growing the remainder.
    gui::Rect untouched{5.0F, 5.0F, 40.0F, 40.0F};
    const gui::Rect none = gui::cut_left(untouched, -10.0F);
    UI_CHECK(near(none.width, 0.0F));
    UI_CHECK(same_rect(untouched, 5.0F, 5.0F, 40.0F, 40.0F));
}

void inset_pads_each_side_and_never_goes_negative() {
    constexpr gui::Rect r{10.0F, 20.0F, 100.0F, 50.0F};
    UI_CHECK(same_rect(gui::inset(r, 7.0F), 17.0F, 27.0F, 86.0F, 36.0F));
    UI_CHECK(same_rect(gui::inset(r, 9.0F, 4.0F), 19.0F, 24.0F, 82.0F, 42.0F));
    UI_CHECK(same_rect(gui::inset(r, gui::Padding{1.0F, 2.0F, 3.0F, 4.0F}),
                       11.0F, 22.0F, 96.0F, 44.0F));

    const gui::Rect crushed = gui::inset(r, 60.0F);
    UI_CHECK(near(crushed.width, 0.0F));
    UI_CHECK(near(crushed.height, 0.0F));
}

void centered_sits_in_the_middle() {
    constexpr gui::Rect r{10.0F, 20.0F, 100.0F, 50.0F};
    UI_CHECK(same_rect(gui::centered(r, 30.0F, 10.0F), 45.0F, 40.0F, 30.0F, 10.0F));
    // Larger than the rect overhangs both sides evenly.
    UI_CHECK(same_rect(gui::centered(r, 120.0F, 50.0F), 0.0F, 20.0F, 120.0F, 50.0F));
}

void columns_share_the_width_and_the_last_meets_the_edge() {
    constexpr gui::Rect r{10.0F, 5.0F, 301.0F, 22.0F};
    const gui::Rect a = gui::column(r, 0U, 3U, 7.0F);
    const gui::Rect b = gui::column(r, 1U, 3U, 7.0F);
    const gui::Rect c = gui::column(r, 2U, 3U, 7.0F);

    // (301 - 2 * 7) / 3 = 95.666...
    UI_CHECK(same_rect(a, 10.0F, 5.0F, 95.667F, 22.0F));
    UI_CHECK(near(b.x, 10.0F + 95.667F + 7.0F));
    UI_CHECK(near(c.x + c.width, 311.0F));
    UI_CHECK(near(b.x - (a.x + a.width), 7.0F));
    UI_CHECK(near(c.x - (b.x + b.width), 7.0F));

    UI_CHECK(near(gui::column(r, 0U, 0U, 7.0F).width, 0.0F));
}

void list_rows_step_down_from_the_scrolled_top_unclamped() {
    constexpr gui::Rect list{10.0F, 100.0F, 200.0F, 50.0F};
    UI_CHECK(same_rect(gui::list_row(list, 0U, 23.0F), 10.0F, 100.0F, 200.0F, 23.0F));
    UI_CHECK(same_rect(gui::list_row(list, 3U, 23.0F, 40.0F), 10.0F, 129.0F, 200.0F, 23.0F));
    // Past the bottom is still a whole row, not a clamped sliver.
    UI_CHECK(same_rect(gui::list_row(list, 5U, 23.0F), 10.0F, 215.0F, 200.0F, 23.0F));
    // Spaced rows keep their height; the gap is between them.
    UI_CHECK(same_rect(gui::list_row(list, 2U, 23.0F, 0.0F, 4.0F), 10.0F, 154.0F, 200.0F,
                       23.0F));
}

void a_cell_is_where_a_column_crosses_a_row() {
    constexpr gui::Rect column{40.0F, 0.0F, 60.0F, 20.0F};
    constexpr gui::Rect row{0.0F, 130.0F, 500.0F, 26.0F};
    UI_CHECK(same_rect(gui::cell(column, row), 40.0F, 130.0F, 60.0F, 26.0F));
}

// The rectangles placed by a point or between two coordinates rather than
// cut from a parent: a handle, a span of time, a playhead, a rubber band.
void point_placed_boxes_sit_where_they_are_asked() {
    UI_CHECK(same_rect(gui::around(50.0F, 40.0F, 10.0F, 6.0F), 45.0F, 37.0F, 10.0F, 6.0F));

    constexpr gui::Rect lane{20.0F, 130.0F, 500.0F, 24.0F};
    UI_CHECK(same_rect(gui::span_x(lane, 60.0F, 95.0F), 60.0F, 130.0F, 35.0F, 24.0F));
    // A span that runs backwards is empty, not negative.
    UI_CHECK(same_rect(gui::span_x(lane, 95.0F, 60.0F), 95.0F, 130.0F, 0.0F, 24.0F));
    UI_CHECK(same_rect(gui::span_y(lane, 140.0F, 150.0F), 20.0F, 140.0F, 500.0F, 10.0F));

    UI_CHECK(same_rect(gui::rule_x(lane, 70.0F), 69.5F, 130.0F, 1.0F, 24.0F));
    UI_CHECK(same_rect(gui::rule_x(lane, 70.0F, 3.0F), 68.5F, 130.0F, 3.0F, 24.0F));
    UI_CHECK(same_rect(gui::rule_y(lane, 140.0F, 2.0F), 20.0F, 139.0F, 500.0F, 2.0F));

    UI_CHECK(same_rect(gui::spanning(90.0F, 20.0F, 30.0F, 70.0F), 30.0F, 20.0F, 60.0F, 50.0F));
    UI_CHECK(same_rect(gui::spanning(30.0F, 70.0F, 90.0F, 20.0F), 30.0F, 20.0F, 60.0F, 50.0F));
}

void boxes_overlap_where_both_are_and_enclose_both() {
    constexpr gui::Rect a{10.0F, 20.0F, 50.0F, 40.0F};
    constexpr gui::Rect b{40.0F, 50.0F, 60.0F, 30.0F};
    UI_CHECK(same_rect(gui::intersection(a, b), 40.0F, 50.0F, 20.0F, 10.0F));
    UI_CHECK(same_rect(gui::intersection(b, a), 40.0F, 50.0F, 20.0F, 10.0F));
    // Apart, they share nothing.
    UI_CHECK(same_rect(gui::intersection(a, gui::Rect{200.0F, 0.0F, 5.0F, 5.0F}),
                       200.0F, 20.0F, 0.0F, 0.0F));
    UI_CHECK(same_rect(gui::enclosing(a, b), 10.0F, 20.0F, 90.0F, 60.0F));
}

void form_rows_follow_the_metrics_rhythm() {
    const gui::Metrics metrics = odd_metrics();
    gui::FormLayout form{gui::Rect{50.0F, 100.0F, 400.0F, 300.0F}, metrics};

    // Content starts `gap` below the top, `inset` in from each side.
    const gui::Rect intro = form.caption();
    UI_CHECK(same_rect(intro, 60.0F, 103.0F, 380.0F, 15.0F));

    const gui::FormRow row = form.row();
    UI_CHECK(same_rect(row.label, 60.0F, 119.0F, 100.0F, 20.0F));
    UI_CHECK(same_rect(row.field, 160.0F, 119.0F, 280.0F, 20.0F));

    const gui::Rect checkbox = form.control();
    UI_CHECK(same_rect(checkbox, 60.0F, 144.0F, 380.0F, 20.0F));
    // What was last handed out survives the gap after it - and a section
    // measures from that row, not from the gap.
    form.space(4.0F);
    UI_CHECK(same_rect(form.last(), 60.0F, 144.0F, 380.0F, 20.0F));

    // The rule is measured from the control's bottom (164), not from the
    // row gap the control left after it.
    const gui::FormSection section = form.section();
    UI_CHECK(same_rect(section.rule, 60.0F, 171.0F, 380.0F, 2.0F));
    UI_CHECK(same_rect(section.heading, 60.0F, 180.0F, 380.0F, 15.0F));

    form.space(11.0F);
    const gui::Rect after = form.control();
    UI_CHECK(near(after.y, 180.0F + 15.0F + 5.0F + 11.0F));
}

void a_section_after_a_caption_sits_as_far_from_it_as_after_a_row() {
    const gui::Metrics metrics = odd_metrics();
    gui::FormLayout after_row{gui::Rect{0.0F, 0.0F, 300.0F, 300.0F}, metrics};
    gui::FormLayout after_caption{gui::Rect{0.0F, 0.0F, 300.0F, 300.0F}, metrics};
    const gui::Rect row = after_row.control();
    const gui::Rect caption = after_caption.caption();
    const gui::FormSection a = after_row.section();
    const gui::FormSection b = after_caption.section();
    UI_CHECK(near(a.rule.y - row.bottom(), 7.0F));
    UI_CHECK(near(b.rule.y - caption.bottom(), 7.0F));
}

void a_note_lines_up_with_the_field_and_a_heading_has_no_rule() {
    const gui::Metrics metrics = odd_metrics();
    gui::FormLayout form{gui::Rect{50.0F, 100.0F, 400.0F, 300.0F}, metrics};
    const gui::Rect heading = form.heading();
    UI_CHECK(same_rect(heading, 60.0F, 103.0F, 380.0F, 15.0F));
    const gui::FormRow row = form.row();
    UI_CHECK(near(row.label.y, 103.0F + 15.0F + 5.0F));
    const gui::Rect note = form.note();
    UI_CHECK(same_rect(note, row.field.x, row.field.bottom() + 5.0F,
                       row.field.width, 15.0F));
    // A note is a caption: the next row follows its caption gap.
    UI_CHECK(near(form.control().y, note.bottom() + 1.0F));
}

void the_label_column_never_overruns_a_narrow_form() {
    gui::FormLayout form{gui::Rect{0.0F, 0.0F, 90.0F, 100.0F}, odd_metrics()};
    const gui::FormRow row = form.row();
    UI_CHECK(near(row.label.width, 70.0F));
    UI_CHECK(near(row.field.width, 0.0F));
}

void extent_is_the_content_height_whatever_the_scroll() {
    const gui::Metrics metrics = odd_metrics();
    gui::FormLayout still{gui::Rect{0.0F, 40.0F, 300.0F, 100.0F}, metrics};
    gui::FormLayout scrolled{gui::Rect{0.0F, 40.0F, 300.0F, 100.0F}, metrics, 33.0F};
    for (gui::FormLayout* form : {&still, &scrolled}) {
        static_cast<void>(form->row());
        static_cast<void>(form->row());
        static_cast<void>(form->caption());
    }
    // gap + row + row_gap + row + row_gap + caption + gap: the trailing
    // caption_gap is spacing, not content.
    constexpr float expected = 3.0F + 20.0F + 5.0F + 20.0F + 5.0F + 15.0F + 3.0F;
    UI_CHECK(near(still.extent(), expected));
    UI_CHECK(near(scrolled.extent(), expected));
    // Scrolling moves the rows, not what they measure.
    UI_CHECK(near(still.cursor() - scrolled.cursor(), 33.0F));
}

struct ScrollHarness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font, theme()};
    gui::ScrollRegion scroll;
    static constexpr gui::Rect viewport{100.0F, 100.0F, 200.0F, 150.0F};

    [[nodiscard]] static gui::Theme theme() {
        gui::Theme theme{};
        theme.metrics = odd_metrics();
        return theme;
    }

    // One frame with the pointer at (x, y) and `wheel` notches; returns the
    // offset the content was laid out at.
    float frame(float x, float y, float wheel, float content_height) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.add_wheel(wheel);
        ctx.new_frame();
        const float offset = scroll.begin(ctx, viewport);
        scroll.end(ctx, content_height);
        return offset;
    }
};

void the_wheel_scrolls_by_the_metrics_step_over_the_viewport() {
    ScrollHarness h;
    UI_CHECK(near(h.frame(150.0F, 150.0F, 0.0F, 400.0F), 0.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, -2.0F, 400.0F), 50.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, 1.0F, 400.0F), 25.0F));
    // Outside the viewport the wheel belongs to something else.
    UI_CHECK(near(h.frame(20.0F, 20.0F, -3.0F, 400.0F), 25.0F));
}

void scrolling_stops_at_the_measured_end() {
    ScrollHarness h;
    static_cast<void>(h.frame(150.0F, 150.0F, 0.0F, 230.0F));
    UI_CHECK(near(h.scroll.max_offset(), 80.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, -10.0F, 230.0F), 80.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, 10.0F, 230.0F), 0.0F));
}

void the_thumb_shows_the_share_in_view_and_where_it_is() {
    ScrollHarness h;
    constexpr gui::Rect track{300.0F, 100.0F, 3.0F, 150.0F};
    static_cast<void>(h.frame(150.0F, 150.0F, 0.0F, 120.0F));
    UI_CHECK(near(h.scroll.thumb(track).height, 0.0F));

    static_cast<void>(h.frame(150.0F, 150.0F, 0.0F, 600.0F));
    UI_CHECK(same_rect(h.scroll.thumb(track), 300.0F, 100.0F, 3.0F, 37.5F));
    static_cast<void>(h.frame(150.0F, 150.0F, -10.0F, 600.0F));
    // 250 of 450: 250 / 450 * (150 - 37.5) below the track's top.
    UI_CHECK(same_rect(h.scroll.thumb(track), 300.0F, 162.5F, 3.0F, 37.5F));
}

void a_list_scrolls_by_its_own_step() {
    ScrollHarness h;
    h.input.new_frame();
    h.input.set_mouse_position(150.0F, 150.0F);
    h.input.add_wheel(-2.0F);
    h.ctx.new_frame();
    UI_CHECK(near(h.scroll.begin(h.ctx, ScrollHarness::viewport, 17.0F), 34.0F));
    h.scroll.end(h.ctx, 400.0F);
}

// The four edges, each `thickness` deep and inside the bounds, as the quads'
// vertex extents: top, bottom, left, right.
void an_outline_draws_four_edges_inside_its_bounds() {
    ScrollHarness h;
    h.ctx.new_frame();
    const std::size_t before = h.list.vertices().size();
    h.ctx.outline(gui::Rect{10.0F, 20.0F, 50.0F, 30.0F}, gui::UiColor{}, 3.0F);
    const auto& vertices = h.list.vertices();
    UI_CHECK(vertices.size() - before == 24U);
    const auto extent = [&](std::size_t quad) {
        float min_x = 1.0e9F, min_y = 1.0e9F, max_x = -1.0e9F, max_y = -1.0e9F;
        for (std::size_t i = before + quad * 6U; i < before + quad * 6U + 6U; ++i) {
            min_x = std::min(min_x, vertices[i].x);
            min_y = std::min(min_y, vertices[i].y);
            max_x = std::max(max_x, vertices[i].x);
            max_y = std::max(max_y, vertices[i].y);
        }
        return gui::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
    };
    UI_CHECK(same_rect(extent(0U), 10.0F, 20.0F, 50.0F, 3.0F));
    UI_CHECK(same_rect(extent(1U), 10.0F, 47.0F, 50.0F, 3.0F));
    UI_CHECK(same_rect(extent(2U), 10.0F, 20.0F, 3.0F, 30.0F));
    UI_CHECK(same_rect(extent(3U), 57.0F, 20.0F, 3.0F, 30.0F));

    h.ctx.outline(gui::Rect{10.0F, 20.0F, 0.0F, 30.0F}, gui::UiColor{});
    UI_CHECK(h.list.vertices().size() - before == 24U);
}

void content_that_shrinks_pulls_the_offset_back() {
    ScrollHarness h;
    static_cast<void>(h.frame(150.0F, 150.0F, 0.0F, 600.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, -10.0F, 600.0F), 250.0F));
    // A section collapsed: the end clamps at once, so the next frame lays out
    // at the new bottom rather than past it.
    static_cast<void>(h.frame(150.0F, 150.0F, 0.0F, 190.0F));
    UI_CHECK(near(h.scroll.offset(), 40.0F));
    UI_CHECK(near(h.frame(150.0F, 150.0F, 0.0F, 190.0F), 40.0F));
    // Content that fits cannot scroll at all.
    static_cast<void>(h.frame(150.0F, 150.0F, -4.0F, 120.0F));
    UI_CHECK(near(h.scroll.offset(), 0.0F));

    h.scroll.reset();
    UI_CHECK(near(h.scroll.offset(), 0.0F));
    UI_CHECK(near(h.scroll.max_offset(), 0.0F));
}

} // namespace

int main() {
    using Case = void (*)();
    constexpr struct {
        const char* name;
        Case run;
    } cases[] = {
        {"edges_and_centre_read_off_the_rect", edges_and_centre_read_off_the_rect},
        {"each_cut_takes_its_edge_and_shrinks_the_rest",
         each_cut_takes_its_edge_and_shrinks_the_rest},
        {"a_cut_larger_than_the_rect_takes_only_what_is_there",
         a_cut_larger_than_the_rect_takes_only_what_is_there},
        {"inset_pads_each_side_and_never_goes_negative",
         inset_pads_each_side_and_never_goes_negative},
        {"centered_sits_in_the_middle", centered_sits_in_the_middle},
        {"columns_share_the_width_and_the_last_meets_the_edge",
         columns_share_the_width_and_the_last_meets_the_edge},
        {"list_rows_step_down_from_the_scrolled_top_unclamped",
         list_rows_step_down_from_the_scrolled_top_unclamped},
        {"a_cell_is_where_a_column_crosses_a_row", a_cell_is_where_a_column_crosses_a_row},
        {"point_placed_boxes_sit_where_they_are_asked",
         point_placed_boxes_sit_where_they_are_asked},
        {"boxes_overlap_where_both_are_and_enclose_both",
         boxes_overlap_where_both_are_and_enclose_both},
        {"form_rows_follow_the_metrics_rhythm", form_rows_follow_the_metrics_rhythm},
        {"a_section_after_a_caption_sits_as_far_from_it_as_after_a_row",
         a_section_after_a_caption_sits_as_far_from_it_as_after_a_row},
        {"a_note_lines_up_with_the_field_and_a_heading_has_no_rule",
         a_note_lines_up_with_the_field_and_a_heading_has_no_rule},
        {"the_label_column_never_overruns_a_narrow_form",
         the_label_column_never_overruns_a_narrow_form},
        {"extent_is_the_content_height_whatever_the_scroll",
         extent_is_the_content_height_whatever_the_scroll},
        {"the_wheel_scrolls_by_the_metrics_step_over_the_viewport",
         the_wheel_scrolls_by_the_metrics_step_over_the_viewport},
        {"scrolling_stops_at_the_measured_end", scrolling_stops_at_the_measured_end},
        {"content_that_shrinks_pulls_the_offset_back",
         content_that_shrinks_pulls_the_offset_back},
        {"the_thumb_shows_the_share_in_view_and_where_it_is",
         the_thumb_shows_the_share_in_view_and_where_it_is},
        {"a_list_scrolls_by_its_own_step", a_list_scrolls_by_its_own_step},
        {"an_outline_draws_four_edges_inside_its_bounds",
         an_outline_draws_four_edges_inside_its_bounds},
    };

    std::size_t failures = 0U;
    for (const auto& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    constexpr std::size_t total = sizeof(cases) / sizeof(cases[0]);
    std::cout << (total - failures) << '/' << total << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
