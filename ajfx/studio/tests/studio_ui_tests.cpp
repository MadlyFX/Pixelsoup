#include "page_view.hpp"
#include "recipes.hpp"
#include "widgets.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Studio's interface, headless: no window and no GPU, synthetic pointer input
// through the shared hot/active model. What is under test is the rule that a
// popover card - the recipe gallery, the block and layer pickers - is drawn
// last and so has to claim the pointer where it is before anything under it
// is drawn, or the control beneath an entry takes the click instead.
namespace {

namespace gui = mediaserver::gui;

using ajfx::studio::Document;
using ajfx::studio::Incoming;
using ajfx::studio::Layer;
using ajfx::studio::ListEntry;
using ajfx::studio::PageInputs;
using ajfx::studio::PageView;
using ajfx::studio::PopoverList;
using ajfx::studio::PopoverShield;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"line " + std::to_string(line) + ": check failed: " + expression};
    }
}

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void begin_frame(gui::InputState& input, gui::UiContext& ctx, float x, float y, bool left_down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, left_down);
    ctx.new_frame();
}

// What the gallery offers: entries under one heading, no search field, as
// Studio's "New" opens it.
[[nodiscard]] std::vector<ListEntry> recipe_entries() {
    return {ListEntry{"Recipes", "Flames", "A fire that licks upward", true, {}},
            ListEntry{"Recipes", "Smoke", "A column that spreads as it rises", true, {}}};
}

// The gallery's second entry, from the layout PopoverList draws: the card's
// top, its 10 inset, a 24 title row and 4 below it, the group heading, then
// one 46 entry.
[[nodiscard]] float second_entry_y(const gui::Rect& card) noexcept {
    return card.y + 10.0F + 28.0F + 26.0F + 46.0F + 22.0F;
}

void an_entry_wins_over_the_control_under_the_card() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect window{0.0F, 0.0F, 1600.0F, 900.0F};
    // A button as wide as the page under the card, as the page header's own
    // controls lie under the gallery.
    const gui::Rect under{600.0F, 60.0F, 400.0F, 300.0F};
    PopoverList gallery;
    gallery.open(610.0F, 82.0F, "Start a new effect", recipe_entries(), false);

    bool fired = false;
    std::optional<std::size_t> picked;
    const auto frame = [&](float x, float y, bool left_down) {
        begin_frame(input, ctx, x, y, left_down);
        {
            const PopoverShield shield{ctx, gallery.covers(ctx.pointer_x(), ctx.pointer_y())};
            fired = ctx.button(gui::ui_id("studio.test.under"), under, "Underneath");
        }
        picked = gallery.draw(ctx, window);
        ctx.draw_overlays(window);
    };

    // One frame away from the card measures it; the pointer then goes to the
    // second entry, over the button.
    frame(40.0F, 500.0F, false);
    CHECK(gallery.is_open());
    const gui::Rect card = gallery.bounds();
    const float y = second_entry_y(card);
    CHECK(under.contains(card.x + 90.0F, y));
    CHECK(gallery.covers(card.x + 90.0F, y));

    frame(card.x + 90.0F, y, true);
    CHECK(!fired);
    CHECK(!picked.has_value());
    frame(card.x + 90.0F, y, false);
    CHECK(!fired);
    CHECK(picked.has_value());
    CHECK(*picked == 1U);
    CHECK(!gallery.is_open());
}

void without_the_shield_the_control_underneath_takes_the_click() {
    // The bug this guards against: the card is drawn last, so a button under
    // it consumes the press first and the entry never sees the release.
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect window{0.0F, 0.0F, 1600.0F, 900.0F};
    const gui::Rect under{600.0F, 60.0F, 400.0F, 300.0F};
    PopoverList gallery;
    gallery.open(610.0F, 82.0F, "Start a new effect", recipe_entries(), false);

    bool fired = false;
    std::optional<std::size_t> picked;
    const auto frame = [&](float x, float y, bool left_down) {
        begin_frame(input, ctx, x, y, left_down);
        fired = ctx.button(gui::ui_id("studio.test.under"), under, "Underneath");
        picked = gallery.draw(ctx, window);
        ctx.draw_overlays(window);
    };

    frame(40.0F, 500.0F, false);
    const gui::Rect card = gallery.bounds();
    const float y = second_entry_y(card);
    frame(card.x + 90.0F, y, true);
    frame(card.x + 90.0F, y, false);
    CHECK(fired);
    CHECK(!picked.has_value());
}

void the_card_leaves_the_rest_of_the_window_alone() {
    // Blocking only where the card is: a control elsewhere still works while
    // the gallery is open, and a press there closes it.
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect window{0.0F, 0.0F, 1600.0F, 900.0F};
    const gui::Rect aside{40.0F, 600.0F, 160.0F, 40.0F};
    PopoverList gallery;
    gallery.open(610.0F, 82.0F, "Start a new effect", recipe_entries(), false);

    bool fired = false;
    const auto frame = [&](float x, float y, bool left_down) {
        begin_frame(input, ctx, x, y, left_down);
        {
            const PopoverShield shield{ctx, gallery.covers(ctx.pointer_x(), ctx.pointer_y())};
            fired = ctx.button(gui::ui_id("studio.test.aside"), aside, "Aside");
        }
        static_cast<void>(gallery.draw(ctx, window));
        ctx.draw_overlays(window);
    };

    frame(300.0F, 500.0F, false);
    CHECK(!gallery.covers(aside.x + 20.0F, aside.y + 20.0F));
    frame(aside.x + 20.0F, aside.y + 20.0F, true);
    CHECK(!gallery.is_open());
    frame(aside.x + 20.0F, aside.y + 20.0F, false);
    CHECK(fired);
}

} // namespace

// The crates layer on a page of its own, so the pointer has one layer to
// find: the pile of crates, the ball beside them, the cut that breaks them
// and the world, in that order in the list.
[[nodiscard]] Document crates_page() {
    Document document;
    std::optional<Layer> layer = ajfx::studio::make_layer_preset(document, "breaking_crates");
    CHECK(layer.has_value());
    document.layers.push_back(std::move(*layer));
    return document;
}

[[nodiscard]] std::vector<std::string> kinds_of(const Layer& layer) {
    std::vector<std::string> kinds;
    for (const ajfx::studio::Block& block : layer.blocks) {
        kinds.push_back(block.kind);
    }
    return kinds;
}

// Holds a palette entry over the page at `py` and lets it go there, as
// dragging one out of the palette does. Two frames: the first works out
// where it would land, the second lets go and lands it.
void drop_from_palette(PageView& page, gui::InputState& input, gui::UiContext& ctx, Document& document,
                       std::string_view block_kind, float py) {
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 900.0F};
    const Incoming incoming{Incoming::Kind::Block, std::string{block_kind}, std::string{block_kind}, std::nullopt};
    for (const bool down : {true, false}) {
        begin_frame(input, ctx, 450.0F, py, down);
        page.set_incoming(&incoming);
        page.draw(ctx, bounds, document, PageInputs{});
        page.draw_overlays(ctx, bounds, document, PageInputs{});
        page.set_incoming(nullptr);
    }
}

// A rigid layer draws its blocks under headings that say what each acts on,
// so a block can only land under its own heading however far the pointer is
// from it. Its kind decides where it is drawn, so a line anywhere else
// would promise a place it cannot stay.
void a_dropped_block_lands_under_its_own_heading() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document = crates_page();
    const std::vector<std::string> before = kinds_of(document.layers[0]);
    // The crates, the cut under them, then the ball: breaking belongs to the
    // Object above it, so it reads between the two.
    CHECK(before == std::vector<std::string>({"object", "break", "object", "gravity", "ground", "appearance",
                                              "color", "size"}));

    // Wind is the world's, and the top of the page is the objects. It lands
    // at the top of the world all the same, before the gravity there.
    PageView page;
    drop_from_palette(page, input, ctx, document, "wind", 6.0F);
    CHECK(kinds_of(document.layers[0]) ==
          std::vector<std::string>({"object", "break", "object", "wind", "gravity", "ground", "appearance",
                                    "color", "size"}));

    // A third object let go at the foot of the page joins the objects, after
    // the two already there, rather than at the end of the list.
    drop_from_palette(page, input, ctx, document, "object", 880.0F);
    CHECK(kinds_of(document.layers[0]) ==
          std::vector<std::string>({"object", "break", "object", "object", "wind", "gravity", "ground",
                                    "appearance", "color", "size"}));

    // And a Break let go at the very top lands under the first object, not
    // above it: breaking has to belong to something, and the object it is
    // drawn under is the one that comes apart.
    drop_from_palette(page, input, ctx, document, "break", 6.0F);
    const std::vector<std::string> after = kinds_of(document.layers[0]);
    CHECK(after[0] == "object" && after[1] == "break" && after[2] == "break");
}

// A particle layer has one scope, so it keeps the flat list it always had:
// a block let go at the foot of the page goes to the end of it.
void a_particle_layer_still_takes_a_block_anywhere() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    std::optional<Layer> layer = ajfx::studio::make_layer_preset(document, "embers");
    CHECK(layer.has_value());
    CHECK(layer->kind == ajfx::studio::LayerKind::Particles);
    document.layers.push_back(std::move(*layer));
    const std::size_t before = document.layers[0].blocks.size();

    PageView page;
    drop_from_palette(page, input, ctx, document, "wind", 880.0F);
    CHECK(document.layers[0].blocks.size() == before + 1U);
    CHECK(document.layers[0].blocks.back().kind == "wind");
}

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"an_entry_wins_over_the_control_under_the_card", an_entry_wins_over_the_control_under_the_card},
        {"a_dropped_block_lands_under_its_own_heading", a_dropped_block_lands_under_its_own_heading},
        {"a_particle_layer_still_takes_a_block_anywhere", a_particle_layer_still_takes_a_block_anywhere},
        {"without_the_shield_the_control_underneath_takes_the_click",
         without_the_shield_the_control_underneath_takes_the_click},
        {"the_card_leaves_the_rest_of_the_window_alone", the_card_leaves_the_rest_of_the_window_alone},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
