#include <mediaserver/editor/color_correction_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

#define COLOR_CHECK(condition)                                              \
    do {                                                                    \
        if (!(condition)) {                                                 \
            throw std::runtime_error{"check failed: " #condition};          \
        }                                                                   \
    } while (false)

struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::ColorCorrectionPanel panel{gui::ui_id("color-correction")};
    std::vector<scene::EffectInstance> effects;

    static constexpr gui::Rect bounds{0.0F, 0.0F, 320.0F, 640.0F};
    static constexpr doc::NodeId layer{20U};
    static constexpr doc::NodeId effect{30U};

    Harness() {
        effects.push_back(scene::EffectInstance{
            effect, layer, scene::EffectKind::Levels, 0, true, 1.0F,
            scene::LevelsParameters{}});
    }

    editor::ColorCorrectionFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, bounds, layer, effects);
    }

    editor::ColorCorrectionFrame type(std::string_view value,
                                      bool enter = true) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.add_text(value);
        ctx.new_frame();
        const editor::ColorCorrectionFrame typed =
            panel.draw(ctx, bounds, layer, effects);
        if (!enter) {
            return typed;
        }
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.add_key(gui::Key::Enter);
        ctx.new_frame();
        return panel.draw(ctx, bounds, layer, effects);
    }

    void click(float x, float y) {
        static_cast<void>(frame(x, y, false));
        static_cast<void>(frame(x, y, true));
        static_cast<void>(frame(x, y, false));
    }
};

void typing_master_black_emits_the_exact_authored_property() {
    Harness harness;
    harness.click(220.0F, 122.0F);
    const editor::ColorCorrectionFrame result = harness.type("0.25");
    COLOR_CHECK(result.edits.size() == 1U);
    COLOR_CHECK(result.edits[0U].effect == Harness::effect);
    COLOR_CHECK(result.edits[0U].property ==
                scene::schema::levels_master_input_black);
    COLOR_CHECK(std::abs(result.edits[0U].value - 0.25) < 1.0e-6);
    COLOR_CHECK(!result.editing);
}

void input_endpoints_never_cross_in_the_editor() {
    Harness harness;
    harness.click(220.0F, 122.0F);
    const editor::ColorCorrectionFrame result = harness.type("4");
    COLOR_CHECK(result.edits.size() == 1U);
    COLOR_CHECK(result.edits[0U].value > 0.99);
    COLOR_CHECK(result.edits[0U].value < 1.0);
}

void levels_rails_are_directly_draggable() {
    Harness harness;
    static_cast<void>(harness.frame(16.0F, 66.0F, false));
    static_cast<void>(harness.frame(16.0F, 66.0F, true));
    const editor::ColorCorrectionFrame dragged =
        harness.frame(74.0F, 66.0F, true);
    static_cast<void>(harness.frame(74.0F, 66.0F, false));
    COLOR_CHECK(dragged.editing);
    COLOR_CHECK(!dragged.edits.empty());
    COLOR_CHECK(dragged.edits.front().property ==
                scene::schema::levels_master_input_black);
    COLOR_CHECK(dragged.edits.front().value > 0.19);
    COLOR_CHECK(dragged.edits.front().value < 0.21);
}

void channel_reset_reports_one_atomic_set_of_changed_fields() {
    Harness harness;
    harness.effects[0U].levels.master =
        scene::LevelsChannelParameters{0.2F, 0.8F, 1.4F, 0.1F, 0.9F};
    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame result =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(result.edits.size() == 5U);
    COLOR_CHECK(result.edits.front().property ==
                scene::schema::levels_master_input_black);
    COLOR_CHECK(result.edits.front().value == 0.0);
    COLOR_CHECK(result.edits.back().property ==
                scene::schema::levels_master_output_white);
    COLOR_CHECK(result.edits.back().value == 1.0);
}

void no_levels_effect_is_an_explicit_empty_state() {
    Harness harness;
    harness.effects.clear();
    const editor::ColorCorrectionFrame result =
        harness.frame(10.0F, 40.0F, false);
    COLOR_CHECK(result.edits.empty());
    COLOR_CHECK(!result.editing);
    COLOR_CHECK(!harness.ctx.typing());
}

void lift_gamma_gain_fields_and_reset_use_the_typed_properties() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::LiftGammaGain;
    harness.effects[0U].lift_gamma_gain.lift =
        scene::Vec3{0.1F, 0.2F, 0.3F};

    harness.click(220.0F, 206.0F);
    const editor::ColorCorrectionFrame typed = harness.type("-2.5");
    COLOR_CHECK(typed.edits.size() == 1U);
    COLOR_CHECK(typed.edits[0U].effect == Harness::effect);
    COLOR_CHECK(typed.edits[0U].property == scene::schema::lift_r);
    COLOR_CHECK(std::abs(typed.edits[0U].value + 2.5) < 1.0e-6);

    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame reset =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(reset.edits.size() == 3U);
    COLOR_CHECK(reset.edits.front().property == scene::schema::lift_r);
    COLOR_CHECK(reset.edits.back().property == scene::schema::lift_b);
    COLOR_CHECK(reset.edits.front().value == 0.0);
    COLOR_CHECK(reset.edits.back().value == 0.0);
}

void lift_gamma_gain_wheels_emit_rgb_edits() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::LiftGammaGain;
    static_cast<void>(harness.frame(67.0F, 117.0F, false));
    static_cast<void>(harness.frame(67.0F, 117.0F, true));
    const editor::ColorCorrectionFrame dragged =
        harness.frame(92.0F, 117.0F, true);
    static_cast<void>(harness.frame(92.0F, 117.0F, false));
    COLOR_CHECK(dragged.editing);
    COLOR_CHECK(dragged.edits.size() == 3U);
    COLOR_CHECK(dragged.edits[0U].property == scene::schema::lift_r);
    COLOR_CHECK(dragged.edits[1U].property == scene::schema::lift_g);
    COLOR_CHECK(dragged.edits[2U].property == scene::schema::lift_b);
}

void contrast_pivot_fields_clamp_and_reset_as_one_gesture() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::ContrastPivot;
    harness.effects[0U].contrast_pivot =
        scene::ContrastPivotParameters{2.0F, 0.3F};

    harness.click(220.0F, 70.0F);
    const editor::ColorCorrectionFrame typed = harness.type("8");
    COLOR_CHECK(typed.edits.size() == 1U);
    COLOR_CHECK(typed.edits[0U].property == scene::schema::contrast);
    COLOR_CHECK(typed.edits[0U].value ==
                scene::max_contrast_pivot_contrast);

    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame reset =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(reset.edits.size() == 2U);
    COLOR_CHECK(reset.edits[0U].property == scene::schema::contrast);
    COLOR_CHECK(reset.edits[0U].value == 1.0);
    COLOR_CHECK(reset.edits[1U].property == scene::schema::pivot);
    COLOR_CHECK(reset.edits[1U].value == 0.5);
}

void channel_mixer_rows_use_matrix_schema_and_row_reset() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::ColorMatrix;
    harness.effects[0U].color_matrix.matrix[0U] = 0.5F;
    harness.effects[0U].color_matrix.matrix[1U] = 0.2F;
    harness.effects[0U].color_matrix.offset.x = 0.1F;

    harness.click(220.0F, 70.0F);
    const editor::ColorCorrectionFrame typed = harness.type("30");
    COLOR_CHECK(typed.edits.size() == 1U);
    COLOR_CHECK(typed.edits[0U].property == scene::schema::m00);
    COLOR_CHECK(typed.edits[0U].value ==
                scene::max_color_matrix_coefficient);

    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame reset =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(reset.edits.size() == 3U);
    COLOR_CHECK(reset.edits[0U].property == scene::schema::m00);
    COLOR_CHECK(reset.edits[0U].value == 1.0);
    COLOR_CHECK(reset.edits[1U].property == scene::schema::m01);
    COLOR_CHECK(reset.edits[1U].value == 0.0);
    COLOR_CHECK(reset.edits[2U].property == scene::schema::offset_r);
    COLOR_CHECK(reset.edits[2U].value == 0.0);
}

void white_balance_fields_clamp_and_reset_to_d65() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::WhiteBalance;
    harness.effects[0U].white_balance.temperature_kelvin = 4'200.0F;
    harness.effects[0U].white_balance.tint = 0.25F;

    harness.click(220.0F, 70.0F);
    const editor::ColorCorrectionFrame typed = harness.type("18000");
    COLOR_CHECK(typed.edits.size() == 1U);
    COLOR_CHECK(typed.edits[0U].property ==
                scene::schema::temperature_kelvin);
    COLOR_CHECK(typed.edits[0U].value ==
                scene::max_white_balance_temperature_kelvin);

    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame reset =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(reset.edits.size() == 2U);
    COLOR_CHECK(reset.edits[0U].property ==
                scene::schema::temperature_kelvin);
    COLOR_CHECK(reset.edits[0U].value ==
                scene::d65_white_balance_temperature_kelvin);
    COLOR_CHECK(reset.edits[1U].property == scene::schema::tint);
    COLOR_CHECK(reset.edits[1U].value == 0.0);
}

void curves_add_point_and_numeric_edit_use_indexed_properties() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::Curves;
    harness.effects[0U].curves.master.points = {
        {0.0F, 0.0F}, {0.4F, 0.8F}, {1.0F, 1.0F}};
    const auto normalized = scene::normalized_curves(
        harness.effects[0U].curves);
    COLOR_CHECK(normalized.has_value());
    harness.effects[0U].curves = *normalized;

    harness.click(220.0F, 184.0F);
    const editor::ColorCorrectionFrame typed = harness.type("0.55");
    COLOR_CHECK(typed.edits.size() == 1U);
    COLOR_CHECK(typed.edits[0U].property == "curve_master_1_x");
    COLOR_CHECK(std::abs(typed.edits[0U].value - 0.55) < 1.0e-6);

    // Clicking empty graph space inserts a control point where the operator
    // clicked; no separate + button stands in for the graph gesture.
    static_cast<void>(harness.frame(240.0F, 135.0F, false));
    const editor::ColorCorrectionFrame add =
        harness.frame(240.0F, 135.0F, true);
    static_cast<void>(harness.frame(240.0F, 135.0F, false));
    COLOR_CHECK(add.edits.size() >= 4U);
    COLOR_CHECK(add.edits[add.edits.size() - 2U].property ==
                "curve_master_2_x");
    COLOR_CHECK(add.edits.back().property == "curve_master_2_y");
}

void curves_points_drag_on_the_graph() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::Curves;
    harness.effects[0U].curves.master.points = {
        {0.0F, 0.0F}, {0.4F, 0.8F}, {1.0F, 1.0F}};
    harness.effects[0U].curves =
        *scene::normalized_curves(harness.effects[0U].curves);
    static_cast<void>(harness.frame(130.0F, 80.0F, false));
    static_cast<void>(harness.frame(130.0F, 80.0F, true));
    const editor::ColorCorrectionFrame dragged =
        harness.frame(160.0F, 100.0F, true);
    static_cast<void>(harness.frame(160.0F, 100.0F, false));
    COLOR_CHECK(dragged.editing);
    COLOR_CHECK(dragged.edits.size() == 2U);
    COLOR_CHECK(dragged.edits[0U].property == "curve_master_1_x");
    COLOR_CHECK(dragged.edits[1U].property == "curve_master_1_y");
}

void curves_reset_and_optional_channel_enable_are_atomic() {
    Harness harness;
    harness.effects[0U].kind = scene::EffectKind::Curves;
    harness.effects[0U].curves.master.points = {
        {0.0F, 0.0F}, {0.4F, 0.8F}, {1.0F, 1.0F}};
    const auto normalized = scene::normalized_curves(
        harness.effects[0U].curves);
    COLOR_CHECK(normalized.has_value());
    harness.effects[0U].curves = *normalized;

    static_cast<void>(harness.frame(285.0F, 44.0F, false));
    static_cast<void>(harness.frame(285.0F, 44.0F, true));
    const editor::ColorCorrectionFrame reset =
        harness.frame(285.0F, 44.0F, false);
    COLOR_CHECK(reset.edits.size() == 1U);
    COLOR_CHECK(reset.edits[0U].property == "curve_master_1_y");
    COLOR_CHECK(std::abs(reset.edits[0U].value - 0.4) < 1.0e-6);

    // Master has one interior point, so its two numeric rows place the Red
    // heading at y=231 in this deterministic panel layout.
    static_cast<void>(harness.frame(285.0F, 247.0F, false));
    static_cast<void>(harness.frame(285.0F, 247.0F, true));
    const editor::ColorCorrectionFrame enable =
        harness.frame(285.0F, 247.0F, false);
    COLOR_CHECK(enable.edits.size() == 4U);
    COLOR_CHECK(enable.edits.front().property == "curve_red_0_x");
    COLOR_CHECK(enable.edits.front().value == 0.0);
    COLOR_CHECK(enable.edits.back().property == "curve_red_1_y");
    COLOR_CHECK(enable.edits.back().value == 1.0);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[]{
    {"typing Master black emits the exact authored property",
     &typing_master_black_emits_the_exact_authored_property},
    {"input endpoints never cross in the editor",
     &input_endpoints_never_cross_in_the_editor},
    {"Levels rails are directly draggable",
     &levels_rails_are_directly_draggable},
    {"channel reset reports one atomic set of changed fields",
     &channel_reset_reports_one_atomic_set_of_changed_fields},
    {"no Levels effect is an explicit empty state",
     &no_levels_effect_is_an_explicit_empty_state},
    {"Lift Gamma Gain fields and reset use typed properties",
     &lift_gamma_gain_fields_and_reset_use_the_typed_properties},
    {"Lift Gamma Gain wheels emit RGB edits",
     &lift_gamma_gain_wheels_emit_rgb_edits},
    {"Contrast Pivot fields clamp and reset as one gesture",
     &contrast_pivot_fields_clamp_and_reset_as_one_gesture},
    {"Channel Mixer rows use matrix schema and row reset",
     &channel_mixer_rows_use_matrix_schema_and_row_reset},
    {"White Balance fields clamp and reset to D65",
     &white_balance_fields_clamp_and_reset_to_d65},
    {"Curves add point and numeric edit use indexed properties",
     &curves_add_point_and_numeric_edit_use_indexed_properties},
    {"Curves points drag on the graph",
     &curves_points_drag_on_the_graph},
    {"Curves reset and optional channel enable are atomic",
     &curves_reset_and_optional_channel_enable_are_atomic},
};

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
