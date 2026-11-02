#include "blast.hpp"
#include "budget.hpp"
#include "compile.hpp"
#include "guides.hpp"
#include "page_view.hpp"
#include "recipes.hpp"
#include "sandbox_tools.hpp"
#include "stage_camera.hpp"
#include "widgets.hpp"
#include "scene_view.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cmath>
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

using ajfx::studio::BlockKind;
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
    // Its gravity and ground are the page's, so the world holds no block.
    CHECK(before == std::vector<std::string>({"object", "break", "object", "appearance", "color", "size"}));

    // Wind is the world's, and the top of the page is the objects. It lands
    // under the world all the same, which is the foot of the layer.
    PageView page;
    drop_from_palette(page, input, ctx, document, "wind", 6.0F);
    CHECK(kinds_of(document.layers[0]) ==
          std::vector<std::string>({"object", "break", "object", "appearance", "color", "size", "wind"}));

    // A third object let go at the foot of the page joins the objects, after
    // the two already there, rather than at the end of the list.
    drop_from_palette(page, input, ctx, document, "object", 880.0F);
    CHECK(kinds_of(document.layers[0]) ==
          std::vector<std::string>({"object", "break", "object", "object", "appearance", "color", "size", "wind"}));

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
    CHECK(document.layers[0].blocks.back().kind == BlockKind::Wind);
}

// A layer with nothing born in it asks what it starts with, as tiles to
// click, rather than leaving a warning with one button. A click adds what the
// tile says and opens the first block; once something is born the tiles go.
void an_empty_layer_starts_from_a_tile() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Rigid, "Yard"));
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "Flames"));
    const ajfx::studio::Uid yard = document.layers[0].uid;
    const ajfx::studio::Uid flames = document.layers[1].uid;
    // Tall enough for both layers to be in view, so every tile can be reached.
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 1600.0F};
    PageView page;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        page.draw(ctx, bounds, document, PageInputs{});
        page.draw_overlays(ctx, bounds, document, PageInputs{});
    };
    // Over the tile, pressed, let go.
    const auto click = [&](const gui::Rect& tile) {
        frame(tile.center_x(), tile.center_y(), false);
        frame(tile.center_x(), tile.center_y(), true);
        frame(tile.center_x(), tile.center_y(), false);
    };

    frame(5.0F, 5.0F, false);
    std::vector<gui::Rect> tiles = page.starter_tiles(yard);
    CHECK(tiles.size() == 4U);
    CHECK(page.starter_tiles(flames).size() == 2U);
    // In view, big enough to hit, and apart.
    for (std::size_t i = 0U; i < tiles.size(); ++i) {
        CHECK(bounds.contains(tiles[i].center_x(), tiles[i].center_y()));
        CHECK(tiles[i].width > 100.0F && tiles[i].height >= 30.0F);
        for (std::size_t j = i + 1U; j < tiles.size(); ++j) {
            const bool apart = tiles[i].right() <= tiles[j].x || tiles[j].right() <= tiles[i].x ||
                               tiles[i].bottom() <= tiles[j].y || tiles[j].bottom() <= tiles[i].y;
            CHECK(apart);
        }
    }

    // "One thing standing there": a fixed object, on the page's ground, opened.
    click(tiles[2]);
    CHECK(kinds_of(document.layers[0]) == std::vector<std::string>({"object"}));
    CHECK(ajfx::studio::choice_of(document.layers[0].blocks[0], "motion") == "fixed");
    CHECK(page.focus() == document.layers[0].blocks[0].uid);
    frame(5.0F, 5.0F, false);
    CHECK(page.starter_tiles(yard).empty());

    // The layer below moved up with it, and still asks. Its flow is a stream.
    tiles = page.starter_tiles(flames);
    CHECK(tiles.size() == 2U);
    click(tiles[0]);
    CHECK(kinds_of(document.layers[1]) == std::vector<std::string>({"stream"}));
    CHECK(page.focus() == document.layers[1].blocks[0].uid);
    frame(5.0F, 5.0F, false);
    CHECK(page.starter_tiles(flames).empty());
    CHECK(document.layers[0].blocks.size() == 1U);
}

// Once something is born, the layer offers what would carry it on as chips in
// a row under its blocks, in place of amber cards worded as faults. A click
// adds the block and opens it, and the chip goes.
void a_layer_offers_its_next_steps_as_chips() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "Flames"));
    document.layers[0].blocks.push_back(*ajfx::studio::make_block(document, "stream"));
    const ajfx::studio::Uid flames = document.layers[0].uid;
    // Tall enough for the page's world rows above both layers.
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 1400.0F};
    PageView page;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        page.draw(ctx, bounds, document, PageInputs{});
        page.draw_overlays(ctx, bounds, document, PageInputs{});
    };
    const auto click = [&](const gui::Rect& chip) {
        frame(chip.center_x(), chip.center_y(), false);
        frame(chip.center_x(), chip.center_y(), true);
        frame(chip.center_x(), chip.center_y(), false);
    };

    // A Stream alone has no look and no fade: two chips, side by side, and none
    // of the starter tiles a layer with nothing born is given.
    frame(5.0F, 5.0F, false);
    std::vector<gui::Rect> chips = page.next_step_chips(flames);
    CHECK(chips.size() == 2U);
    CHECK(page.starter_tiles(flames).empty());
    for (const gui::Rect& chip : chips) {
        CHECK(bounds.contains(chip.center_x(), chip.center_y()));
        // The headless font has no advance, so the width is the room beside
        // the words alone; the words' own width is checked by looking.
        CHECK(chip.width >= 40.0F && chip.height >= 24.0F);
    }
    CHECK(chips[0].y == chips[1].y && chips[0].right() <= chips[1].x);

    // The first is a look: an Appearance, opened. What is left is the fade.
    click(chips[0]);
    CHECK(kinds_of(document.layers[0]) == std::vector<std::string>({"stream", "appearance"}));
    CHECK(page.focus() == document.layers[0].blocks[1].uid);
    frame(5.0F, 5.0F, false);
    chips = page.next_step_chips(flames);
    CHECK(chips.size() == 1U);
    click(chips[0]);
    CHECK(kinds_of(document.layers[0]) == std::vector<std::string>({"stream", "appearance", "fade"}));
    frame(5.0F, 5.0F, false);
    CHECK(page.next_step_chips(flames).empty());

    // A rigid layer that holds only a standing thing asks for what falls on it.
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Rigid, "Yard"));
    const ajfx::studio::Uid yard = document.layers[1].uid;
    ajfx::studio::Block crate = *ajfx::studio::make_block(document, "object");
    crate.set("motion", ajfx::studio::choice("fixed"));
    ajfx::studio::insert_block(document.layers[1], 0U, std::move(crate));
    frame(5.0F, 5.0F, false);
    chips = page.next_step_chips(yard);
    CHECK(chips.size() == 1U);
    CHECK(bounds.contains(chips[0].center_x(), chips[0].center_y()));
    click(chips[0]);
    // The page gives it gravity and a ground, so the Stream comes alone.
    CHECK(kinds_of(document.layers[1]) == std::vector<std::string>({"object", "stream"}));
    // The Stream is the one opened, the rest only come with it.
    CHECK(page.focus() == document.layers[1].blocks[1].uid);
}

// Where a palette click lands is one rule, which the page marks and the
// palette names: the layer being worked on, else the last one worked on, else
// the last on the page. It used to be worked out in three places from three
// variables, so what was marked, what the palette greyed and what a click
// added to could be three different layers.
void the_target_layer_is_the_one_a_click_adds_to() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "A"));
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "B"));
    const ajfx::studio::Uid a = document.layers[0].uid;
    const ajfx::studio::Uid b = document.layers[1].uid;
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 900.0F};
    PageView page;
    const auto frame = [&](bool escape) {
        input.new_frame();
        input.set_mouse_position(5.0F, 5.0F);
        input.set_mouse_button(gui::MouseButton::Left, false);
        if (escape) {
            input.add_key(gui::Key::Escape);
        }
        ctx.new_frame();
        page.draw(ctx, bounds, document, PageInputs{});
        page.draw_overlays(ctx, bounds, document, PageInputs{});
    };
    const auto from_palette = [&](std::string_view block) {
        page.add(document, Incoming{Incoming::Kind::Block, std::string{block}, std::string{block}, std::nullopt});
    };
    const auto restore = [&](ajfx::studio::Uid layer, std::vector<ajfx::studio::Uid> folded) {
        ajfx::studio::PageViewState view;
        view.layer = layer;
        view.folded = std::move(folded);
        page.restore(view);
    };

    // Nothing has been worked on: the last layer on the page.
    frame(false);
    CHECK(page.working_layer(document) == 0U);
    CHECK(page.target_layer(document) == b);
    from_palette("gravity");
    CHECK(kinds_of(document.layers[1]) == std::vector<std::string>({"gravity"}));
    CHECK(page.working_layer(document) == b && page.target_layer(document) == b);

    // Back to another layer, which is then what a click adds to.
    restore(a, {});
    frame(false);
    CHECK(page.working_layer(document) == a && page.target_layer(document) == a);
    from_palette("ground");
    CHECK(kinds_of(document.layers[0]) == std::vector<std::string>({"ground"}));
    CHECK(kinds_of(document.layers[1]) == std::vector<std::string>({"gravity"}));

    // Esc stops working on anything, and the target is still the layer last
    // worked on, since that is where a click goes.
    frame(true);
    CHECK(page.working_layer(document) == 0U);
    CHECK(page.target_layer(document) == a);
    from_palette("wind");
    CHECK(kinds_of(document.layers[0]) == std::vector<std::string>({"ground", "wind"}));

    // Folding it does not move the target either.
    restore(a, {a});
    frame(false);
    CHECK(page.working_layer(document) == 0U && page.target_layer(document) == a);

    // A layer taken off the page leaves the last one.
    CHECK(ajfx::studio::remove_layer(document, a));
    frame(false);
    CHECK(page.target_layer(document) == b);
    // And on an empty page there is none.
    CHECK(ajfx::studio::remove_layer(document, b));
    frame(false);
    CHECK(page.target_layer(document) == 0U);
}

// A layer's heading carries only what matters. "Live" is what every layer is
// and says nothing, so it shows on the target layer, or while the pointer is
// over the heading, and a baked layer says "Baked" always. The kind chip is
// what it looks like, a button: it opens both kinds, and choosing the other
// changes the layer unless a block of its own is in the way.
void a_layer_heading_shows_what_matters() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "A"));
    document.layers.push_back(ajfx::studio::make_layer(document, ajfx::studio::LayerKind::Particles, "B"));
    const ajfx::studio::Uid a = document.layers[0].uid;
    const ajfx::studio::Uid b = document.layers[1].uid;
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 900.0F};
    PageView page;
    // The kind chip is under Advanced: the expert view.
    PageInputs expert;
    expert.expert = true;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        page.draw(ctx, bounds, document, expert);
        page.draw_overlays(ctx, bounds, document, expert);
        ctx.draw_overlays(bounds);
    };
    const auto click = [&](float x, float y) {
        frame(x, y, false);
        frame(x, y, true);
        frame(x, y, false);
    };
    // Where a chip's menu starts: hung 2 below the chip, and the menu 6 below
    // that, as the shared menu hangs under its anchor.
    const auto menu_top = [](const gui::Rect& chip) { return chip.bottom() + 2.0F + 6.0F; };

    // Both say what moves them. Live shows on the target, the last layer, and
    // not on the other.
    frame(5.0F, 5.0F, false);
    CHECK(page.header_chip(a, "kind").has_value() && page.header_chip(b, "kind").has_value());
    CHECK(page.header_chip(a, "what").has_value() && page.header_chip(b, "what").has_value());
    CHECK(page.header_chip(b, "bake").has_value() && !page.header_chip(a, "bake").has_value());

    // The pointer over a heading brings it, and leaving takes it away.
    const gui::Rect kind = *page.header_chip(a, "kind");
    frame(kind.center_x(), kind.center_y(), false);
    CHECK(page.header_chip(a, "bake").has_value());
    frame(5.0F, 5.0F, false);
    CHECK(!page.header_chip(a, "bake").has_value());
    // A baked layer says so whatever the pointer does.
    document.layers[0].baked = true;
    frame(5.0F, 5.0F, false);
    CHECK(page.header_chip(a, "bake").has_value());
    document.layers[0].baked = false;

    // The kind chip opens a menu of both kinds; the second makes it rigid,
    // with the world a rigid layer has.
    frame(5.0F, 5.0F, false);
    const gui::Rect chip = *page.header_chip(a, "kind");
    click(chip.center_x(), chip.center_y());
    CHECK(ctx.menu_open(ajfx::studio::part_id(0U, "layer.kind")));
    const float second = menu_top(chip) + gui::menu_row_height * 1.5F;
    frame(chip.x + 20.0F, second, true);
    frame(chip.x + 20.0F, second, false);
    CHECK(!ctx.menu_open(ajfx::studio::part_id(0U, "layer.kind")));
    CHECK(document.layers[0].kind == ajfx::studio::LayerKind::Rigid && document.layers[0].world.has_value());

    // The kind it already is, the second entry now, changes nothing.
    frame(5.0F, 5.0F, false);
    const gui::Rect again = *page.header_chip(a, "kind");
    click(again.center_x(), again.center_y());
    const float already = menu_top(again) + gui::menu_row_height * 1.5F;
    frame(again.x + 20.0F, already, true);
    frame(again.x + 20.0F, already, false);
    CHECK(document.layers[0].kind == ajfx::studio::LayerKind::Rigid && document.layers[0].world.has_value());
    // And the first, particles, takes it back.
    frame(5.0F, 5.0F, false);
    const gui::Rect back = *page.header_chip(a, "kind");
    click(back.center_x(), back.center_y());
    const float first = menu_top(back) + gui::menu_row_height * 0.5F;
    frame(back.x + 20.0F, first, true);
    frame(back.x + 20.0F, first, false);
    CHECK(document.layers[0].kind == ajfx::studio::LayerKind::Particles && !document.layers[0].world.has_value());

    // A block the other kind cannot hold stays in the way: nothing changes,
    // and nothing it holds is lost.
    ajfx::studio::insert_block(document.layers[1], 0U, *ajfx::studio::make_block(document, "swirl"));
    frame(5.0F, 5.0F, false);
    const gui::Rect stuck = *page.header_chip(b, "kind");
    click(stuck.center_x(), stuck.center_y());
    const float rigid = menu_top(stuck) + gui::menu_row_height * 1.5F;
    frame(stuck.x + 20.0F, rigid, true);
    frame(stuck.x + 20.0F, rigid, false);
    CHECK(document.layers[1].kind == ajfx::studio::LayerKind::Particles);
    CHECK(kinds_of(document.layers[1]) == std::vector<std::string>({"swirl"}));
}

// An object in a solid layer is blown up from its own heading, in one click:
// the chip is there while the block is open or under the pointer, and a click
// cuts the object, sets off an explosion at it and adds its fire, sparks,
// smoke and dust. The explosion is then what is worked on, so its handle is
// the one the preview offers.
void explode_this_is_one_click_on_an_object() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    std::optional<Layer> crate = ajfx::studio::make_layer_preset(document, "crate");
    CHECK(crate.has_value() && crate->kind == ajfx::studio::LayerKind::Rigid);
    document.layers.push_back(std::move(*crate));
    CHECK(document.layers[0].blocks[0].kind == BlockKind::Object);
    const ajfx::studio::Uid object = document.layers[0].blocks[0].uid;
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 1400.0F};
    PageView page;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        page.draw(ctx, bounds, document, PageInputs{});
        page.draw_overlays(ctx, bounds, document, PageInputs{});
    };

    // Closed, and away from the pointer, it says nothing of exploding.
    frame(5.0F, 5.0F, false);
    CHECK(!page.block_chip(object, "explode").has_value());
    // Open, it offers it.
    page.open_block(object);
    frame(5.0F, 5.0F, false);
    const std::optional<gui::Rect> chip = page.block_chip(object, "explode");
    CHECK(chip.has_value());
    CHECK(bounds.contains(chip->center_x(), chip->center_y()));
    frame(chip->center_x(), chip->center_y(), false);
    frame(chip->center_x(), chip->center_y(), true);
    frame(chip->center_x(), chip->center_y(), false);

    CHECK(document.layers.size() == 7U);
    CHECK(document.layers[1].what == ajfx::studio::LayerWhat::Fire);
    CHECK(document.layers[2].what == ajfx::studio::LayerWhat::Sparks);
    CHECK(document.layers[3].what == ajfx::studio::LayerWhat::Smoke);
    CHECK(document.layers[4].what == ajfx::studio::LayerWhat::Dust);
    CHECK(document.layers[5].what == ajfx::studio::LayerWhat::Dust);
    CHECK(document.layers[6].what == ajfx::studio::LayerWhat::Fire);
    const std::vector<std::string> kinds = kinds_of(document.layers[0]);
    CHECK(kinds[0] == "object" && kinds[1] == "break" && kinds.back() == "explosion");
    CHECK(page.focus() == document.layers[0].blocks.back().uid);
}

// The explosion's dot in the preview is dragged across the floor, and where
// it is let go is where the blast goes off.
void dragging_the_explosion_handle_moves_the_blast() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    std::optional<Layer> crate = ajfx::studio::make_layer_preset(document, "crate");
    CHECK(crate.has_value());
    document.layers.push_back(std::move(*crate));
    const std::optional<ajfx::studio::Exploded> made = ajfx::studio::explode_this(document, document.layers[0].uid);
    CHECK(made.has_value());
    const ajfx::studio::GuideOptions options{.focus = made->explosion};
    const gui::Rect view{0.0F, 0.0F, 800.0F, 600.0F};
    const ajfx::studio::StageCamera camera{
        ajfx::studio::CameraPose{.eye = ajfx::Vec3{0.0F, 3.0F, 6.0F}, .target = ajfx::Vec3{0.0F, 0.25F, 0.0F}}, view};
    ajfx::studio::HandleDrags drags;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        const ajfx::studio::GuideSet guides = ajfx::studio::build_guides(document, options);
        drags.draw(ctx, view, camera, document, guides);
    };

    const ajfx::studio::GuideSet guides = ajfx::studio::build_guides(document, options);
    const ajfx::studio::Handle* at = nullptr;
    for (const ajfx::studio::Handle& handle : guides.handles) {
        if (handle.block == made->explosion && handle.control == "at") {
            at = &handle;
        }
    }
    CHECK(at != nullptr);
    const ajfx::Vec3 goal{1.5F, at->at.y, -0.5F};
    const std::optional<gui::UiPoint> from = camera.project(at->at);
    const std::optional<gui::UiPoint> to = camera.project(goal);
    CHECK(from.has_value() && to.has_value());
    CHECK(view.contains(from->x, from->y) && view.contains(to->x, to->y));

    frame(from->x, from->y, false);
    frame(from->x, from->y, true);
    CHECK(drags.dragging());
    frame(to->x, to->y, true);
    frame(to->x, to->y, false);
    CHECK(!drags.dragging());

    const ajfx::studio::Value placed = ajfx::studio::get(*document.block(made->explosion), "at");
    CHECK(std::fabs(placed.x - goal.x) < 0.02F && std::fabs(placed.y - goal.y) < 1.0e-4F &&
          std::fabs(placed.z - goal.z) < 0.02F);
    const ajfx::studio::Compiled compiled = ajfx::studio::compile(document, ajfx::studio::Purpose::Preview);
    CHECK(compiled.effect.has_value() && compiled.effect->blasts.size() == 1U);
    const ajfx::Vec3 centre = compiled.effect->blasts[0].center;
    CHECK(std::fabs(centre.x - goal.x) < 0.02F && std::fabs(centre.z - goal.z) < 0.02F);
}

// The sandbox strip is reached with the pointer: a click on "Explode here"
// turns it on, and a click in the picture then writes an Explosion where the
// floor was clicked, at the playhead, into the target layer -- one block, as
// one edit. A drag across the picture is an orbit, and writes nothing. The die
// deals the page new seeds.
void the_sandbox_strip_explodes_where_the_picture_is_clicked() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document;
    std::optional<Layer> crate = ajfx::studio::make_layer_preset(document, "crate");
    CHECK(crate.has_value());
    document.layers.push_back(std::move(*crate));
    const ajfx::studio::Uid layer = document.layers[0].uid;
    const std::size_t blocks_before = document.layers[0].blocks.size();
    const gui::Rect view{0.0F, 0.0F, 800.0F, 600.0F};
    const ajfx::studio::StageCamera camera{
        ajfx::studio::CameraPose{.eye = ajfx::Vec3{0.0F, 3.0F, 6.0F}, .target = ajfx::Vec3{0.0F, 0.25F, 0.0F}}, view};
    ajfx::studio::SandboxTools tools;
    ajfx::studio::SandboxScene scene;
    scene.seconds = 1.25F;
    scene.layer = layer;
    std::optional<ajfx::studio::SandboxResult> result;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        static_cast<void>(tools.draw_strip(ctx, view.x + 10.0F, view.y + 44.0F, document, 0U));
        const gui::Interaction hit = ctx.grab(gui::ui_id("studio.test.view"), view);
        if (std::optional<ajfx::studio::SandboxResult> made = tools.use_click(ctx, hit, camera, document, scene)) {
            result = made;
        }
    };
    const auto click = [&](float x, float y) {
        frame(x, y, false);
        frame(x, y, true);
        frame(x, y, false);
    };

    frame(500.0F, 500.0F, false);
    CHECK(tools.tool() == ajfx::studio::SandboxTool::Select);
    const std::optional<gui::Rect> key = tools.button(ajfx::studio::SandboxTool::Explode);
    CHECK(key.has_value() && view.contains(key->center_x(), key->center_y()));
    click(key->center_x(), key->center_y());
    CHECK(tools.tool() == ajfx::studio::SandboxTool::Explode);
    // Pressing the key is not a click in the picture.
    CHECK(!result.has_value() && document.layers[0].blocks.size() == blocks_before);

    // A drag across the picture orbits and writes nothing.
    const ajfx::Vec3 goal{1.5F, 0.0F, -0.5F};
    const std::optional<gui::UiPoint> at = camera.project(goal);
    CHECK(at.has_value() && view.contains(at->x, at->y));
    frame(at->x, at->y, false);
    frame(at->x, at->y, true);
    frame(at->x + 40.0F, at->y, true);
    frame(at->x + 40.0F, at->y, false);
    CHECK(!result.has_value() && document.layers[0].blocks.size() == blocks_before);

    // A click writes the explosion there, at the playhead.
    click(at->x, at->y);
    CHECK(result.has_value() && result->tool == ajfx::studio::SandboxTool::Explode && result->layer == layer);
    CHECK(document.layers[0].blocks.size() == blocks_before + 1U);
    const ajfx::studio::Block& made = document.layers[0].blocks.back();
    CHECK(made.uid == result->block && made.kind == BlockKind::Explosion);
    const ajfx::studio::Value placed = ajfx::studio::get(made, "at");
    CHECK(std::fabs(placed.x - goal.x) < 0.02F && std::fabs(placed.y) < 1.0e-3F && std::fabs(placed.z - goal.z) < 0.02F);
    CHECK(std::fabs(ajfx::studio::number_of(made, "time") - 1.25F) < 1.0e-6F);
    const ajfx::studio::Compiled compiled = ajfx::studio::compile(document, ajfx::studio::Purpose::Preview);
    CHECK(compiled.effect.has_value() && compiled.effect->blasts.size() == 1U);
    CHECK(std::fabs(compiled.effect->blasts[0].time - 1.25F) < 1.0e-6F);

    // The key again goes back to selecting.
    click(key->center_x(), key->center_y());
    CHECK(tools.tool() == ajfx::studio::SandboxTool::Select);

    // The die re-rolls the page.
    const std::optional<gui::Rect> die = tools.dice();
    CHECK(die.has_value() && view.contains(die->center_x(), die->center_y()));
    const std::uint32_t seed = document.layers[0].seed;
    click(die->center_x(), die->center_y());
    CHECK(document.layers[0].seed != seed);
}

// The budget row's button, pressed as an artist would: it asks for a run,
// and the run -- measured here by a stand-in for the GPU -- comes back as
// one page with more particles. Under way it is a Stop; once a run found the
// page over, it offers to scale down.
void the_budget_button_maximizes_the_page() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    std::optional<Document> made = ajfx::studio::make_recipe("campfire");
    CHECK(made.has_value());
    Document document = std::move(*made);
    const gui::Rect bounds{0.0F, 0.0F, 900.0F, 1400.0F};
    PageView page;
    ajfx::studio::BudgetView view;
    const auto frame = [&](float x, float y, bool down) {
        PageInputs inputs{};
        inputs.budget = &view;
        begin_frame(input, ctx, x, y, down);
        page.draw(ctx, bounds, document, inputs);
        page.draw_overlays(ctx, bounds, document, inputs);
    };
    const auto click = [&](const gui::Rect& at) {
        frame(at.center_x(), at.center_y(), false);
        frame(at.center_x(), at.center_y(), true);
        frame(at.center_x(), at.center_y(), false);
    };

    frame(5.0F, 5.0F, false);
    CHECK(!page.take_budget_request().has_value());
    const std::optional<gui::Rect> button = page.budget_button();
    CHECK(button.has_value() && bounds.contains(button->center_x(), button->center_y()));
    click(*button);
    const std::optional<ajfx::studio::BudgetRequest> asked = page.take_budget_request();
    CHECK(asked.has_value() && !asked->stop && !asked->allow_down && asked->options.keep_look);
    CHECK(!page.take_budget_request().has_value());

    // The run, as the application drives it: a cost in proportion to the
    // particles the page holds, a quarter of the budget as it stands.
    const ajfx::studio::Document before = document;
    const double particles =
        static_cast<double>(ajfx::studio::compile(before, ajfx::studio::Purpose::Preview).total_capacity());
    const double per = static_cast<double>(document.budget_ms) * 0.25 / particles;
    ajfx::studio::BudgetRun run{document, asked->options, ajfx::studio::density_limit(document, asked->options),
                                asked->allow_down};
    view.running = true;
    while (const std::optional<double> factor = run.next()) {
        view.status = run.progress();
        frame(5.0F, 5.0F, false);
        const ajfx::studio::Compiled probe = ajfx::studio::compile(run.page_at(*factor), ajfx::studio::Purpose::Preview);
        run.report(per * static_cast<double>(probe.total_capacity()));
        // The page is not touched while it is measured.
        CHECK(document == before);
    }
    const ajfx::studio::BudgetOutcome outcome = run.outcome();
    CHECK(outcome.page.has_value() && outcome.factor > 3.0);
    document = *outcome.page;
    view = ajfx::studio::BudgetView{false, outcome.words, false};
    frame(5.0F, 5.0F, false);
    const double after =
        static_cast<double>(ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).total_capacity());
    CHECK(after > particles * 3.0);

    // Under way, the same button stops it.
    view.running = true;
    frame(5.0F, 5.0F, false);
    click(*page.budget_button());
    const std::optional<ajfx::studio::BudgetRequest> stop = page.take_budget_request();
    CHECK(stop.has_value() && stop->stop);
    // Over, it offers to scale down.
    view = ajfx::studio::BudgetView{false, "Over: uses 6 of 4 ms", true};
    frame(5.0F, 5.0F, false);
    click(*page.budget_button());
    const std::optional<ajfx::studio::BudgetRequest> down = page.take_budget_request();
    CHECK(down.has_value() && !down->stop && down->allow_down);
}

void scene_multi_edit_preserves_untouched_vector_axes() {
    for (bool night : {false, true}) {
        for (float scale : {1.0F, 1.25F, 1.5F}) {
            gui::InputState input;
            gui::UiDrawList list;
            list.set_scale(scale);
            const gui::FontAtlas font = gui::FontAtlas::empty();
            gui::UiContext ctx{input, list, font, ajfx::studio::studio_theme(night)};
            ajfx::studio::SceneView view;
            Document document = ajfx::studio::empty_scene();
            const auto first = ajfx::studio::add_object(document);
            const auto second = ajfx::studio::add_object(document);
            auto object = [&](auto uid) -> ajfx::studio::Object& {
                return std::get<ajfx::studio::Object>(document.scene->item(uid)->data);
            };
            object(first).position = {2, 3, 4};
            object(second).position = {7, 8, 9};
            view.select(first);
            view.select(second, true);
            const auto frame = [&](float x, float y, bool down, std::string_view text = {}, bool enter = false) {
                input.new_frame();
                input.set_mouse_position(x, y);
                input.set_mouse_button(gui::MouseButton::Left, down);
                input.add_text(text);
                if (enter) { input.add_key(gui::Key::Enter); }
                ctx.new_frame();
                view.draw_inspector(ctx, gui::Rect{800, 0, 450, 1400}, document, {});
                ctx.draw_overlays(gui::Rect{0, 0, 1280, 1440});
            };
            const auto click = [&](float x, float y) {
                frame(x, y, false); frame(x, y, true); frame(x, y, false);
            };
            // The actual inspector's X field. Resolve it to the primary
            // object's existing X: that is still an edit of a mixed field.
            click(960, 205);
            frame(960, 205, false, "7", true);
            CHECK(object(first).position == ajfx::Vec3{7, 3, 4});
            CHECK(object(second).position == ajfx::Vec3{7, 8, 9});
            // A whole-vector reset deliberately resets all three axes.
            click(1230, 205);
            CHECK(object(first).position == ajfx::Vec3{0, 1, 0});
            CHECK(object(second).position == ajfx::Vec3{0, 1, 0});
            // Each item has its own seed default, even in a multi-selection.
            document.scene->item(first)->seed = 123U;
            document.scene->item(second)->seed = 456U;
            click(1230, 132);
            CHECK(document.scene->item(first)->seed == first);
            CHECK(document.scene->item(second)->seed == second);
        }
    }
}

void scene_color_picker_is_one_live_gesture() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    ajfx::studio::SceneView view;
    Document document = ajfx::studio::empty_scene();
    const auto uid = ajfx::studio::add_object(document);
    view.select(uid);
    const auto frame = [&](float x, float y, bool down, bool escape = false) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        if (escape) { input.add_key(gui::Key::Escape); }
        ctx.new_frame();
        view.draw_inspector(ctx, gui::Rect{800, 0, 450, 1400}, document, {});
        ctx.draw_overlays(gui::Rect{0, 0, 1280, 1440});
    };
    frame(1000, 445, false);
    CHECK(!view.gesture_live());
    frame(1000, 445, true); frame(1000, 445, false);
    CHECK(ctx.color_open(ajfx::studio::part_id(uid, "tint")));
    frame(1000, 445, false);
    CHECK(view.gesture_live());
    frame(1000, 445, false, true);
    frame(1000, 445, false);
    CHECK(!view.gesture_live());
}

void scene_creation_uses_the_shipping_add_menu() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    ajfx::studio::SceneView view;
    Document document = ajfx::studio::new_scene();
    CHECK(document.scene && document.scene->items.size() == 1U);
    CHECK(document.scene->items.front().name == "Ground");
    const gui::Rect window{0, 0, 1280, 720};
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        {
            const PopoverShield shield{ctx, view.covers(x, y)};
            view.draw_tree(ctx, gui::Rect{0, 0, 300, 700}, document);
            view.draw_inspector(ctx, gui::Rect{800, 0, 450, 700}, document, {});
        }
        view.draw_overlays(ctx, window, document);
        ctx.draw_overlays(window);
    };
    frame(500, 500, false);
    frame(50, 25, true); frame(50, 25, false);
    frame(60, 98, true); frame(60, 98, false);
    CHECK(document.scene && document.scene->items.size() == 2U);
    CHECK(std::get<ajfx::studio::Object>(document.scene->items.back().data).motion == ajfx::studio::ObjectMotion::Dynamic);
    CHECK(document.layers.empty());
    CHECK(ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).effect);
    frame(50, 25, true); frame(50, 25, false);
    frame(60, 144, true); frame(60, 144, false);
    CHECK(document.scene->items.size() == 3U);
    const auto* system = std::get_if<ajfx::studio::ParticleSystem>(&document.scene->items.back().data);
    CHECK(system && system->sources.size() == 1U);
    CHECK(ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).effect);
    frame(50, 25, true); frame(50, 25, false);
    frame(60, 282, true); frame(60, 282, false);
    CHECK(document.scene->items.size() == 4U);
    CHECK(document.scene->items.back().name == "Ground");
    CHECK(std::get<ajfx::studio::Object>(document.scene->items.back().data).motion == ajfx::studio::ObjectMotion::Static);
    CHECK(ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).effect);
}

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"scene_multi_edit_preserves_untouched_vector_axes", scene_multi_edit_preserves_untouched_vector_axes},
        {"scene_color_picker_is_one_live_gesture", scene_color_picker_is_one_live_gesture},
        {"scene_creation_uses_the_shipping_add_menu", scene_creation_uses_the_shipping_add_menu},
        {"an_entry_wins_over_the_control_under_the_card", an_entry_wins_over_the_control_under_the_card},
        {"a_dropped_block_lands_under_its_own_heading", a_dropped_block_lands_under_its_own_heading},
        {"a_particle_layer_still_takes_a_block_anywhere", a_particle_layer_still_takes_a_block_anywhere},
        {"an_empty_layer_starts_from_a_tile", an_empty_layer_starts_from_a_tile},
        {"a_layer_offers_its_next_steps_as_chips", a_layer_offers_its_next_steps_as_chips},
        {"the_target_layer_is_the_one_a_click_adds_to", the_target_layer_is_the_one_a_click_adds_to},
        {"a_layer_heading_shows_what_matters", a_layer_heading_shows_what_matters},
        {"explode_this_is_one_click_on_an_object", explode_this_is_one_click_on_an_object},
        {"the_budget_button_maximizes_the_page", the_budget_button_maximizes_the_page},
        {"dragging_the_explosion_handle_moves_the_blast", dragging_the_explosion_handle_moves_the_blast},
        {"the_sandbox_strip_explodes_where_the_picture_is_clicked",
         the_sandbox_strip_explodes_where_the_picture_is_clicked},
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
