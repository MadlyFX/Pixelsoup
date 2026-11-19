#include <mediaserver/show/pointer_overlay.hpp>

#include "show_test_support.hpp"

#include <array>
#include <limits>

namespace {

namespace show = mediaserver::show;

[[nodiscard]] show::PointerOverlay picture(std::uint64_t frame, float x) {
    show::PointerOverlay overlay{};
    overlay.control_frame = frame;
    overlay.dot = show::PointerPoint{x, 17.0F};
    return overlay;
}

void a_well_formed_picture_is_valid() {
    show::PointerOverlay overlay{};
    overlay.dot = show::PointerPoint{-40.0F, 1100.0F};
    overlay.points = {{1.0F, 2.0F}, {3.0F, 4.0F}, {9.0F, 9.0F}};
    overlay.stroke_ends = {2U, 3U};
    SHOW_CHECK(show::valid(overlay));
    SHOW_CHECK(!overlay.empty());
    SHOW_CHECK(show::PointerOverlay{}.empty());
    SHOW_CHECK(show::valid(show::PointerOverlay{}));
}

void strokes_must_cover_every_point_once() {
    show::PointerOverlay overlay{};
    overlay.points = {{1.0F, 2.0F}, {3.0F, 4.0F}, {9.0F, 9.0F}};
    overlay.stroke_ends = {2U};
    SHOW_CHECK(!show::valid(overlay)); // a point belongs to no stroke
    overlay.stroke_ends = {2U, 2U, 3U};
    SHOW_CHECK(!show::valid(overlay)); // an empty stroke
    overlay.stroke_ends = {2U, 4U};
    SHOW_CHECK(!show::valid(overlay)); // past the last point
    overlay.stroke_ends = {3U};
    SHOW_CHECK(show::valid(overlay));
}

void numbers_out_of_bounds_are_refused() {
    show::PointerOverlay overlay{};
    overlay.dot = show::PointerPoint{std::numeric_limits<float>::quiet_NaN(),
                                     0.0F};
    SHOW_CHECK(!show::valid(overlay));
    overlay.dot = show::PointerPoint{0.0F, 2.0F * show::max_pointer_coordinate};
    SHOW_CHECK(!show::valid(overlay));
    overlay.dot.reset();
    overlay.points = {{std::numeric_limits<float>::infinity(), 0.0F}};
    overlay.stroke_ends = {1U};
    SHOW_CHECK(!show::valid(overlay));
}

void budgets_are_enforced() {
    show::PointerOverlay overlay{};
    overlay.points.assign(show::max_pointer_points + 1U,
                          show::PointerPoint{1.0F, 1.0F});
    overlay.stroke_ends = {
        static_cast<std::uint32_t>(overlay.points.size())};
    SHOW_CHECK(!show::valid(overlay));
    overlay.points.resize(show::max_pointer_strokes + 1U);
    overlay.stroke_ends.clear();
    for (std::uint32_t end = 1U; end <= overlay.points.size(); ++end) {
        overlay.stroke_ends.push_back(end);
    }
    SHOW_CHECK(!show::valid(overlay));
    overlay.points.resize(show::max_pointer_strokes);
    overlay.stroke_ends.resize(show::max_pointer_strokes);
    SHOW_CHECK(show::valid(overlay));
}

void a_picture_waits_for_its_frame() {
    show::PointerOverlayChannel channel;
    SHOW_CHECK(channel.at(100U) == nullptr);
    SHOW_CHECK(channel.accept(picture(103U, 5.0F)));
    // Presenting 101 and 102, the picture authored for 103 is not due yet.
    SHOW_CHECK(channel.at(101U) == nullptr);
    SHOW_CHECK(channel.at(102U) == nullptr);
    const show::PointerOverlay* shown = channel.at(103U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 5.0F);
    // It holds while no newer one is due.
    shown = channel.at(110U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 5.0F);
}

void the_newest_due_picture_wins() {
    show::PointerOverlayChannel channel;
    SHOW_CHECK(channel.accept(picture(10U, 1.0F)));
    SHOW_CHECK(channel.accept(picture(11U, 2.0F)));
    SHOW_CHECK(channel.accept(picture(13U, 3.0F)));
    const show::PointerOverlay* shown = channel.at(12U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 2.0F);
    shown = channel.at(13U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 3.0F);
}

void a_later_picture_for_the_same_frame_replaces_it() {
    show::PointerOverlayChannel channel;
    SHOW_CHECK(channel.accept(picture(20U, 1.0F)));
    SHOW_CHECK(channel.accept(picture(20U, 4.0F)));
    const show::PointerOverlay* shown = channel.at(20U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 4.0F);
}

void an_older_picture_never_rewinds_the_wall() {
    show::PointerOverlayChannel channel;
    SHOW_CHECK(channel.accept(picture(30U, 1.0F)));
    SHOW_CHECK(channel.at(30U) != nullptr);
    SHOW_CHECK(!channel.accept(picture(29U, 9.0F)));
    const show::PointerOverlay* shown = channel.at(31U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 1.0F);
}

void a_full_channel_drops_the_oldest() {
    show::PointerOverlayChannel channel;
    const std::size_t sent = show::PointerOverlayChannel::capacity + 3U;
    for (std::size_t index = 0U; index < sent; ++index) {
        SHOW_CHECK(channel.accept(picture(100U + index,
                                          static_cast<float>(index))));
    }
    // The first three were dropped unseen; the earliest frame still held is
    // the fourth one sent.
    SHOW_CHECK(channel.at(102U) == nullptr);
    const show::PointerOverlay* shown = channel.at(103U);
    SHOW_CHECK(shown != nullptr && shown->dot->x == 3.0F);
    shown = channel.at(100U + sent);
    SHOW_CHECK(shown != nullptr
               && shown->dot->x == static_cast<float>(sent - 1U));
}

void an_empty_picture_clears_the_wall() {
    show::PointerOverlayChannel channel;
    SHOW_CHECK(channel.accept(picture(40U, 1.0F)));
    show::PointerOverlay cleared{};
    cleared.control_frame = 42U;
    SHOW_CHECK(channel.accept(cleared));
    SHOW_CHECK(channel.at(41U) != nullptr && !channel.at(41U)->empty());
    const show::PointerOverlay* shown = channel.at(42U);
    SHOW_CHECK(shown != nullptr && shown->empty());
}

} // namespace

int main() {
    using show::test::Case;
    const std::array cases{
        Case{"a_well_formed_picture_is_valid", a_well_formed_picture_is_valid},
        Case{"strokes_must_cover_every_point_once",
             strokes_must_cover_every_point_once},
        Case{"numbers_out_of_bounds_are_refused",
             numbers_out_of_bounds_are_refused},
        Case{"budgets_are_enforced", budgets_are_enforced},
        Case{"a_picture_waits_for_its_frame", a_picture_waits_for_its_frame},
        Case{"the_newest_due_picture_wins", the_newest_due_picture_wins},
        Case{"a_later_picture_for_the_same_frame_replaces_it",
             a_later_picture_for_the_same_frame_replaces_it},
        Case{"an_older_picture_never_rewinds_the_wall",
             an_older_picture_never_rewinds_the_wall},
        Case{"a_full_channel_drops_the_oldest",
             a_full_channel_drops_the_oldest},
        Case{"an_empty_picture_clears_the_wall",
             an_empty_picture_clears_the_wall},
    };
    return show::test::run(cases);
}
