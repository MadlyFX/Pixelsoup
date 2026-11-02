#include "compile.hpp"
#include "guides.hpp"
#include "scene.hpp"
#include "scene_view.hpp"
#include "stage_camera.hpp"
#include "widgets.hpp"

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
// through the shared hot/active model. A popover card - the Add menu - is
// drawn last and so has to claim the pointer where it is before anything
// under it is drawn, or the control beneath an entry takes the click instead.
// The scene's tree, inspector and viewport handles are driven the same way.
namespace {

namespace gui = mediaserver::gui;

using ajfx::studio::Document;
using ajfx::studio::ListEntry;
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

// What a popover list offers: entries under one heading, no search field.
[[nodiscard]] std::vector<ListEntry> sample_entries() {
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
    gallery.open(610.0F, 82.0F, "Start a new effect", sample_entries(), false);

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
    gallery.open(610.0F, 82.0F, "Start a new effect", sample_entries(), false);

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
    gallery.open(610.0F, 82.0F, "Start a new effect", sample_entries(), false);

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

// An object's position handle in the viewport, dragged with the pointer,
// moves the authored object: the drag writes the lowered Object block, and
// the scene takes it back as the item's own position, which then compiles.
void dragging_an_objects_handle_moves_the_object() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    Document document = ajfx::studio::new_scene();
    const auto uid = ajfx::studio::add_object(document);
    auto& object = std::get<ajfx::studio::Object>(document.scene->item(uid)->data);
    object.position = ajfx::Vec3{0.0F, 0.5F, 0.0F};
    const Document before = document;
    Document lowered = ajfx::studio::lower_scene(document);
    const ajfx::studio::GuideOptions options{.focus = ajfx::studio::handle_block(document, uid), .layer = uid};
    const gui::Rect view{0.0F, 0.0F, 800.0F, 600.0F};
    const ajfx::studio::StageCamera camera{
        ajfx::studio::CameraPose{.eye = ajfx::Vec3{0.0F, 3.0F, 6.0F}, .target = ajfx::Vec3{0.0F, 0.25F, 0.0F}}, view};
    ajfx::studio::HandleDrags drags;
    const auto frame = [&](float x, float y, bool down) {
        begin_frame(input, ctx, x, y, down);
        const ajfx::studio::GuideSet guides = ajfx::studio::build_guides(lowered, options);
        drags.draw(ctx, view, camera, lowered, guides);
    };

    const ajfx::studio::GuideSet guides = ajfx::studio::build_guides(lowered, options);
    const ajfx::studio::Handle* at = nullptr;
    for (const ajfx::studio::Handle& handle : guides.handles) {
        if (handle.block == options.focus && handle.control == "at") {
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

    CHECK(ajfx::studio::adopt_handle_edit(document, lowered, uid));
    const auto& moved = std::get<ajfx::studio::Object>(document.scene->item(uid)->data);
    CHECK(std::fabs(moved.position.x - goal.x) < 0.02F && std::fabs(moved.position.y - 0.5F) < 1.0e-4F &&
          std::fabs(moved.position.z - goal.z) < 0.02F);
    CHECK(moved.size == object.size);
    // Nothing else of the scene moved, and taking the same drag again is no edit.
    Document expected = before;
    std::get<ajfx::studio::Object>(expected.scene->item(uid)->data).position = moved.position;
    CHECK(document == expected);
    CHECK(!ajfx::studio::adopt_handle_edit(document, lowered, uid));
    CHECK(document.layers.empty());
    CHECK(ajfx::studio::compile(document, ajfx::studio::Purpose::Preview).effect);
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
            // Typed, then Enter a frame later: a text field drops text that
            // arrives in the same input batch as the Enter that closes it.
            click(960, 205);
            frame(960, 205, false, "7");
            frame(960, 205, false, {}, true);
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

// Too narrow for three readable fields side by side, a vector takes one axis
// to a row under its label, and the middle row edits Y alone.
void a_narrow_inspector_gives_each_axis_its_own_row() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    ajfx::studio::SceneView view;
    Document document = ajfx::studio::empty_scene();
    const auto uid = ajfx::studio::add_object(document);
    auto& object = std::get<ajfx::studio::Object>(document.scene->item(uid)->data);
    object.position = {2, 3, 4};
    view.select(uid);
    const auto frame = [&](float x, float y, bool down, std::string_view text = {}, bool enter = false) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.add_text(text);
        if (enter) { input.add_key(gui::Key::Enter); }
        ctx.new_frame();
        view.draw_inspector(ctx, gui::Rect{800, 0, 200, 1400}, document, {});
        ctx.draw_overlays(gui::Rect{0, 0, 1280, 1440});
    };
    // Position's label row starts at 192; X, Y and Z follow at 220, 250, 280.
    frame(860, 264, false);
    frame(860, 264, true);
    frame(860, 264, false);
    frame(860, 264, false, "5");
    frame(860, 264, false, {}, true);
    CHECK(object.position == ajfx::Vec3{2, 5, 4});
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

} // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"scene_multi_edit_preserves_untouched_vector_axes", scene_multi_edit_preserves_untouched_vector_axes},
        {"a_narrow_inspector_gives_each_axis_its_own_row", a_narrow_inspector_gives_each_axis_its_own_row},
        {"scene_color_picker_is_one_live_gesture", scene_color_picker_is_one_live_gesture},
        {"scene_creation_uses_the_shipping_add_menu", scene_creation_uses_the_shipping_add_menu},
        {"dragging_an_objects_handle_moves_the_object", dragging_an_objects_handle_moves_the_object},
        {"an_entry_wins_over_the_control_under_the_card", an_entry_wins_over_the_control_under_the_card},
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
