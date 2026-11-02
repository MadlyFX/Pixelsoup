#pragma once

#include "catalog.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Studio's own small controls, built on the media server's UI toolkit and
// drawn in its theme so Studio reads as part of the same family.
namespace ajfx::studio {

namespace gui = mediaserver::gui;

// Workspace-only night dim; authored colors and viewport pixels are untouched.
[[nodiscard]] gui::Theme studio_theme(bool night);

// Ids for a control of a block: stable across frames, distinct per part.
[[nodiscard]] gui::UiId part_id(std::uint64_t owner, std::string_view name, std::uint32_t part = 0U);

// The icon a category is drawn with.
void draw_category_icon(gui::UiContext& ctx, Category category, float cx, float cy, gui::UiColor color);

// Six dots: the grip a block is dragged by.
void draw_grip(gui::UiContext& ctx, float cx, float cy, gui::UiColor color);

// Marks the shared icons do not have, in the same 14 px square: a die, for
// another random seed, and a burst of rays, for an explosion.
void draw_dice(gui::UiContext& ctx, float cx, float cy, gui::UiColor color);
void draw_burst(gui::UiContext& ctx, float cx, float cy, gui::UiColor color);

// A tool key as editor::tool_key draws one -- a soft rounded key, eased
// lighter under the pointer, lit in `engaged_color` while its mode is on --
// with one of the marks above. Returns true on a click.
using GlyphDraw = void (*)(gui::UiContext&, float, float, gui::UiColor);
[[nodiscard]] bool glyph_key(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds, GlyphDraw glyph, bool engaged,
                             std::string_view tooltip, gui::UiColor engaged_color);

// Choices as a row of pills, the chosen one lit. Falls back to a dropdown
// when they do not fit. Returns true on the frame the choice changed.
[[nodiscard]] bool choice_chips(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds,
                                std::span<const std::string_view> labels, std::size_t& selected);

// A sliding switch. Returns true on the frame it flipped.
[[nodiscard]] bool switch_toggle(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds, bool& value);

// `text` broken into lines no wider than `width`.
[[nodiscard]] std::vector<std::string> wrap_text(const gui::UiContext& ctx, std::string_view text, float width);

// A label at a multiple of the UI font, falling back to the UI font.
void label_sized(gui::UiContext& ctx, const gui::Rect& bounds, std::string_view text, gui::UiColor color,
                 float multiple);
[[nodiscard]] float text_width_sized(const gui::UiContext& ctx, std::string_view text, float multiple);

// "12,480".
[[nodiscard]] std::string thousands(std::uint64_t value);

// A floating list - the block picker, the layer presets, the recipes - as a
// rounded card with a search field. Entries are grouped under headings.
struct ListEntry final {
    std::string group;
    std::string title;
    std::string description;
    bool enabled{true};
    std::optional<Category> icon;
};

class PopoverList final {
public:
    // Opens at `anchor` (the card's top left, kept on screen).
    void open(float x, float y, std::string title, std::vector<ListEntry> entries, bool searchable = true);
    void close() noexcept { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept { return open_; }
    // The card's rectangle last frame, for blocking what lies under it.
    [[nodiscard]] const gui::Rect& bounds() const noexcept { return bounds_; }
    // Open and over this point, so the card owns the pointer here.
    [[nodiscard]] bool covers(float x, float y) const noexcept { return open_ && bounds_.contains(x, y); }

    // Draws it; returns the chosen entry's index (into the entries it was
    // opened with) on the frame one is picked. Escape or a click outside
    // closes it.
    [[nodiscard]] std::optional<std::size_t> draw(gui::UiContext& ctx, const gui::Rect& window);

private:
    bool open_{false};
    bool fresh_{false};
    float x_{};
    float y_{};
    std::string title_;
    std::vector<ListEntry> entries_;
    bool searchable_{true};
    std::string search_;
    std::size_t highlight_{0U};
    gui::Rect bounds_{};
    gui::ScrollRegion scroll_;
};

// Holds the pointer for a popover card drawn over the window. The card is
// drawn last, so every control under it has to be told to ignore the pointer
// before it is drawn, or a press on an entry lands on the control instead and
// the entry the artist aimed at never sees the click. Only where the card is:
// the rest of the window still hovers, and a press outside still closes the
// popover. Released just before the card itself is drawn.
class PopoverShield final {
public:
    PopoverShield(gui::UiContext& ctx, bool covered);
    PopoverShield(const PopoverShield&) = delete;
    PopoverShield& operator=(const PopoverShield&) = delete;
    PopoverShield(PopoverShield&&) = delete;
    PopoverShield& operator=(PopoverShield&&) = delete;
    ~PopoverShield() { release(); }

    void release() noexcept;

private:
    gui::UiContext* ctx_{nullptr};
};

} // namespace ajfx::studio
