#pragma once

#include "document.hpp"
#include "page_view.hpp"
#include "widgets.hpp"

#include <mediaserver/gui/form_layout.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

// The palette: everything a page can be built from, on show down the left -
// ready-made layers, then every block by what it is for, then the raw
// modules under the hood. Seeing what there is gives ideas. An entry is
// dragged onto the page, where a line shows where it will land, or clicked
// to add it to the layer being worked on.
namespace ajfx::studio {

class Palette final {
public:
    // The width the palette takes when open, and folded to its rail.
    static constexpr float open_width = 236.0F;
    static constexpr float rail_width = 40.0F;

    // Draws the palette (or, folded, its rail) in `bounds`. Call before the
    // page draws, and pass held() to the page. `target` is the layer a click
    // adds to (PageView::target_layer): the heading names it, and entries it
    // cannot hold are greyed.
    void draw(gui::UiContext& ctx, const gui::Rect& bounds, const Document& document, Uid target);
    // After every panel: lets go of an entry once the pointer is up.
    void end_frame(gui::UiContext& ctx);

    // The entry being dragged, or null.
    [[nodiscard]] const Incoming* held() const noexcept { return dragging_ ? &pressed_entry_ : nullptr; }
    // An entry clicked this frame, to add where the artist is working.
    [[nodiscard]] std::optional<Incoming> take_clicked();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    void set_open(bool open) noexcept { open_ = open; }
    // Sections folded shut, by heading, for the session.
    [[nodiscard]] std::vector<std::string> folded() const { return {folded_.begin(), folded_.end()}; }
    void set_folded(const std::vector<std::string>& folded) { folded_ = {folded.begin(), folded.end()}; }

private:
    struct Entry final {
        Incoming incoming;
        std::string description;
        // Words the search matches beyond the title and description.
        std::string keywords;
        bool unique{false};
        // The layer kinds this entry belongs in, and why it is not in the
        // others, for an entry greyed against the layer being worked on.
        std::uint8_t layers{in_any_layer};
        std::string only_in;
    };
    struct Section final {
        std::string heading;
        std::vector<Entry> entries;
    };

    void build();

    std::vector<Section> sections_;
    std::set<std::string> folded_{"Under the hood"};
    bool open_{true};
    std::string search_;
    gui::ScrollRegion scroll_;

    bool pressed_{false};
    bool dragging_{false};
    float press_x_{};
    float press_y_{};
    Incoming pressed_entry_;
    std::optional<Incoming> clicked_;
};

} // namespace ajfx::studio
