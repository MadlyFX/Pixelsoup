#include "director_scene_authoring.hpp"
#include "mask_edit_integration.hpp"

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Shapes drawn on Program Output, as the Director authors them: a mask of
// exactly the shape drawn on the layer it was drawn on, and - drawn on bare
// picture - a new card born with its mask in one replacement document.
namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;
namespace show = mediaserver::show;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define SHAPE_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(double a, double b) { return std::abs(a - b) < 1.0e-5; }

const scene::SceneId stage{1U};
const doc::NodeId layer{10U};

[[nodiscard]] doc::Root stage_with_layer() {
    doc::Node stage_node{};
    stage_node.id = stage;
    stage_node.type = std::string{schema::stage_type};
    stage_node.properties.emplace(std::string{schema::scene_name},
                                  std::string{"Shapes"});
    doc::Node object{};
    object.id = layer;
    object.type = std::string{schema::object_type};
    object.properties.emplace(std::string{schema::scene_id},
                              static_cast<std::int64_t>(stage.value()));
    return doc::Root{}.set(stage_node).set(object);
}

[[nodiscard]] std::vector<scene::MaskInstance> masks_of(
    const doc::Root& document) {
    scene::SceneModel model;
    model.reset(document);
    return model.masks(stage);
}

[[nodiscard]] double real_of(const doc::Node& node, std::string_view name) {
    const auto found = node.properties.find(name);
    SHAPE_CHECK(found != node.properties.end());
    const auto* const value = std::get_if<double>(&found->second);
    SHAPE_CHECK(value != nullptr);
    return *value;
}

[[nodiscard]] editor::MaskEditIntent drawn(scene::MaskShape shape,
                                           doc::NodeId target = layer) {
    return editor::MaskEditIntent{
        editor::MaskEditRequest{editor::MaskDrawIntent{
            stage, target, scene::MaskCoordinateSpace::SourceLayer,
            std::move(shape)}},
        7U};
}

void a_shape_drawn_on_a_layer_is_a_mask_of_that_shape() {
    const doc::Root base = stage_with_layer();
    const auto ellipse = app::plan_mask_edit_integration(
        base,
        drawn(scene::EllipseMaskShape{
            scene::MaskBounds{0.2F, 0.3F, 0.6F, 0.9F}}),
        11U);
    SHAPE_CHECK(ellipse.has_value());
    const auto first = masks_of(ellipse.value().document);
    SHAPE_CHECK(first.size() == 1U);
    SHAPE_CHECK(first[0U].source == ellipse.value().mask);
    SHAPE_CHECK(first[0U].target == layer);
    SHAPE_CHECK(first[0U].kind == scene::MaskKind::Ellipse);
    SHAPE_CHECK(first[0U].name == "Ellipse Mask");
    SHAPE_CHECK(first[0U].order == 0);
    SHAPE_CHECK(near(first[0U].bounds.left, 0.2) &&
                near(first[0U].bounds.top, 0.3) &&
                near(first[0U].bounds.right, 0.6) &&
                near(first[0U].bounds.bottom, 0.9));
    // Ids carry on past the mask just made.
    SHAPE_CHECK(ellipse.value().next_node > ellipse.value().mask.value());

    // A second shape goes on top of the first, and is a rectangle because
    // one was drawn.
    const auto rectangle = app::plan_mask_edit_integration(
        ellipse.value().document,
        drawn(scene::RectangleMaskShape{
            scene::MaskBounds{0.1F, 0.15F, 0.45F, 0.5F}}),
        ellipse.value().next_node);
    SHAPE_CHECK(rectangle.has_value());
    const auto both = masks_of(rectangle.value().document);
    SHAPE_CHECK(both.size() == 2U);
    const scene::MaskInstance& second =
        both[0U].source == rectangle.value().mask ? both[0U] : both[1U];
    SHAPE_CHECK(second.kind == scene::MaskKind::Rectangle);
    SHAPE_CHECK(second.name == "Rectangle Mask");
    SHAPE_CHECK(second.order == 1);
    SHAPE_CHECK(near(second.bounds.right, 0.45));
}

void a_curve_keeps_its_points_in_the_order_they_were_placed() {
    scene::BezierMaskShape curve{{
        scene::MaskPointGeometry{0.1F, 0.2F, 0.05F, 0.2F, 0.3F, 0.2F},
        scene::MaskPointGeometry{0.9F, 0.25F, 0.7F, 0.1F, 0.95F, 0.4F},
        scene::MaskPointGeometry{0.5F, 0.8F, 0.6F, 0.85F, 0.4F, 0.75F},
    }};
    const auto planned =
        app::plan_mask_edit_integration(stage_with_layer(), drawn(curve), 11U);
    SHAPE_CHECK(planned.has_value());
    const auto masks = masks_of(planned.value().document);
    SHAPE_CHECK(masks.size() == 1U);
    SHAPE_CHECK(masks[0U].kind == scene::MaskKind::Bezier);
    SHAPE_CHECK(masks[0U].name == "Bezier Mask");
    SHAPE_CHECK(masks[0U].points.size() == 3U);
    SHAPE_CHECK(near(masks[0U].points[0U].x, 0.1) &&
                near(masks[0U].points[0U].out_x, 0.3));
    SHAPE_CHECK(near(masks[0U].points[1U].x, 0.9) &&
                near(masks[0U].points[1U].in_y, 0.1));
    SHAPE_CHECK(near(masks[0U].points[2U].y, 0.8) &&
                near(masks[0U].points[2U].out_x, 0.4));

    // Two points do not enclose anything.
    curve.points.pop_back();
    const auto refused =
        app::plan_mask_edit_integration(stage_with_layer(), drawn(curve), 11U);
    SHAPE_CHECK(!refused.has_value());
    SHAPE_CHECK(refused.error().code ==
                scene::MaskAuthoringErrorCode::MinimumBezierPoints);
}

void a_shape_for_a_missing_layer_or_the_wrong_space_is_refused() {
    const doc::Root base = stage_with_layer();
    const auto missing = app::plan_mask_edit_integration(
        base, drawn(scene::RectangleMaskShape{}, doc::NodeId{99U}), 11U);
    SHAPE_CHECK(!missing.has_value());
    SHAPE_CHECK(missing.error().code ==
                scene::MaskAuthoringErrorCode::TargetMissing);

    // A layer's masks are in the layer's own coordinates, never the canvas's.
    editor::MaskDrawIntent on_canvas{
        stage, layer, scene::MaskCoordinateSpace::AuthoredCanvas,
        scene::RectangleMaskShape{}};
    const auto wrong = app::plan_mask_edit_integration(
        base, editor::MaskEditIntent{editor::MaskEditRequest{on_canvas}, 7U},
        11U);
    SHAPE_CHECK(!wrong.has_value());
    SHAPE_CHECK(wrong.error().code ==
                scene::MaskAuthoringErrorCode::InvalidShape);
    SHAPE_CHECK(masks_of(base).empty());
}

void a_shape_on_bare_picture_is_a_card_born_with_its_mask() {
    const doc::Root base = stage_with_layer();
    editor::AddIntent add{};
    add.kind = editor::AddKind::Background;
    add.scene = stage;
    add.gesture = 31U;
    add.position = scene::Vec3{1.5F, 2.5F, -3.0F};
    add.rotation = scene::Vec3{0.125F, 0.0F, 0.0F};
    add.scale = scene::Vec3{3.25F, 1.75F, 1.0F};
    add.name = "Ellipse";
    const doc::NodeId card_id{20U};
    const doc::Node card = app::make_added_node(
        card_id, add, show::FrameRate{24U, 1U}, scene::IcvfxCameraSettings{});
    // The card is the size and place drawn, named for its shape, rather than
    // the two-metre Background a click in the Library makes.
    SHAPE_CHECK(near(real_of(card, schema::scale_x), 3.25));
    SHAPE_CHECK(near(real_of(card, schema::scale_y), 1.75));
    SHAPE_CHECK(near(real_of(card, schema::position_y), 2.5));
    SHAPE_CHECK(near(real_of(card, schema::rotation_yaw), 0.125));
    SHAPE_CHECK(std::get<std::string>(
                    card.properties.at(std::string{schema::group_name})) ==
                "Ellipse");

    const auto planned = app::plan_shape_card(
        base, card, stage, scene::EllipseMaskShape{}, 21U);
    SHAPE_CHECK(planned.has_value());
    const doc::Root& after = planned.value().document;
    SHAPE_CHECK(after.contains(card_id));
    const auto masks = masks_of(after);
    SHAPE_CHECK(masks.size() == 1U);
    SHAPE_CHECK(masks[0U].target == card_id);
    SHAPE_CHECK(masks[0U].kind == scene::MaskKind::Ellipse);
    // The whole card: the ellipse touches each of its sides.
    SHAPE_CHECK(near(masks[0U].bounds.left, 0.0) &&
                near(masks[0U].bounds.top, 0.0) &&
                near(masks[0U].bounds.right, 1.0) &&
                near(masks[0U].bounds.bottom, 1.0));
    SHAPE_CHECK(planned.value().mask == masks[0U].source);
    SHAPE_CHECK(planned.value().mask.value() >= 21U);
    SHAPE_CHECK(planned.value().next_node > planned.value().mask.value());
    // Exactly the card and its mask are new; nothing that was there moved.
    SHAPE_CHECK(after.size() == base.size() + 2U);
    SHAPE_CHECK(doc::diff(base, after).changes.size() == 2U);

    // A counter that has not passed the card's id still cannot reuse it.
    const auto low = app::plan_shape_card(base, card, stage,
                                          scene::EllipseMaskShape{}, 5U);
    SHAPE_CHECK(low.has_value());
    SHAPE_CHECK(low.value().mask != card_id);
    SHAPE_CHECK(low.value().mask != layer);

    // An id already in the document is not a new card.
    doc::Node taken = card;
    taken.id = layer;
    const auto refused = app::plan_shape_card(base, taken, stage,
                                              scene::EllipseMaskShape{}, 21U);
    SHAPE_CHECK(!refused.has_value());
    SHAPE_CHECK(masks_of(base).empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[] = {
    {"a shape drawn on a layer is a mask of that shape",
     &a_shape_drawn_on_a_layer_is_a_mask_of_that_shape},
    {"a curve keeps its points in the order they were placed",
     &a_curve_keeps_its_points_in_the_order_they_were_placed},
    {"a shape for a missing layer or the wrong space is refused",
     &a_shape_for_a_missing_layer_or_the_wrong_space_is_refused},
    {"a shape on bare picture is a card born with its mask",
     &a_shape_on_bare_picture_is_a_card_born_with_its_mask},
};

}  // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
