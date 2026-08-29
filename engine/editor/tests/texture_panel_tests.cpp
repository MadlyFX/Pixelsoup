#include <mediaserver/editor/texture_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

#define TEXTURE_CHECK(condition)                                            \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};         \
        }                                                                   \
    } while (false)

struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::TexturePanel panel{gui::ui_id("texture")};
    std::vector<scene::EffectInstance> effects;

    static constexpr gui::Rect bounds{0.0F, 0.0F, 320.0F, 260.0F};
    static constexpr doc::NodeId layer{20U};
    static constexpr doc::NodeId effect{30U};

    Harness() {
        scene::EffectInstance value{};
        value.source = effect;
        value.target = layer;
        value.kind = scene::EffectKind::FilmGrain;
        value.film_grain = scene::FilmGrainParameters{
            0.25F, 2.0F, 0.1F, 17U,
            scene::FilmGrainMode::Monochrome};
        value.value = value.film_grain.amount;
        effects.push_back(value);
    }

    editor::TexturePanelFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, bounds, layer, effects);
    }

    void press(float x, float y) {
        static_cast<void>(frame(x, y, false));
        static_cast<void>(frame(x, y, true));
    }

    editor::TexturePanelFrame release(float x, float y) {
        return frame(x, y, false);
    }

    editor::TexturePanelFrame type(std::string_view value) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.add_text(value);
        ctx.new_frame();
        static_cast<void>(panel.draw(ctx, bounds, layer, effects));
        input.new_frame();
        input.add_key(gui::Key::Enter);
        ctx.new_frame();
        return panel.draw(ctx, bounds, layer, effects);
    }
};

void exact_seed_and_normalized_amount_are_typed_whole_effect_edits() {
    Harness harness;
    harness.press(220.0F, 46.0F);
    static_cast<void>(harness.release(220.0F, 46.0F));
    const editor::TexturePanelFrame amount = harness.type("2");
    TEXTURE_CHECK(amount.edit.has_value());
    TEXTURE_CHECK(amount.edit->effect == Harness::effect);
    TEXTURE_CHECK(amount.edit->film_grain.amount == 1.0F);
    TEXTURE_CHECK(amount.edit->film_grain.seed == 17U);

    harness.press(220.0F, 127.0F);
    static_cast<void>(harness.release(220.0F, 127.0F));
    const editor::TexturePanelFrame seed = harness.type("4294967295");
    TEXTURE_CHECK(seed.edit.has_value());
    TEXTURE_CHECK(seed.edit->film_grain.seed == 4'294'967'295U);
    TEXTURE_CHECK(std::abs(seed.edit->film_grain.amount - 0.25F) < 1.0e-6F);
}

void mode_and_reset_are_explicit_typed_actions() {
    Harness harness;
    harness.press(220.0F, 154.0F);
    const editor::TexturePanelFrame mode = harness.release(220.0F, 154.0F);
    TEXTURE_CHECK(mode.edit.has_value());
    TEXTURE_CHECK(mode.edit->film_grain.mode == scene::FilmGrainMode::RGB);
    TEXTURE_CHECK(mode.edit->film_grain.seed == 17U);

    harness.press(220.0F, 181.0F);
    const editor::TexturePanelFrame reset = harness.release(220.0F, 181.0F);
    TEXTURE_CHECK(reset.edit.has_value());
    TEXTURE_CHECK(reset.edit->film_grain == scene::FilmGrainParameters{});
}

void missing_effect_is_an_explicit_empty_state() {
    Harness harness;
    harness.effects.clear();
    const editor::TexturePanelFrame frame = harness.frame(10.0F, 40.0F, false);
    TEXTURE_CHECK(!frame.edit.has_value());
    TEXTURE_CHECK(!frame.editing);
    TEXTURE_CHECK(!harness.ctx.typing());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{{
    "exact seed and normalized amount are typed whole-effect edits",
    &exact_seed_and_normalized_amount_are_typed_whole_effect_edits}, {
    "mode and reset are explicit typed actions",
    &mode_and_reset_are_explicit_typed_actions}, {
    "missing effect is an explicit empty state",
    &missing_effect_is_an_explicit_empty_state}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (std::size(cases) - failures) << '/' << std::size(cases)
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
