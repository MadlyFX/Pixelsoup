#include "test_support.hpp"

// Guides in the preview: what a focused block shows and lets be dragged.
namespace {

using namespace studio_test;

void guides_show_the_focused_block_with_handles() {
    Document document = recipe("campfire");
    document.scale = 2.0F;
    Layer& flames = layer_named(document, "Flames");
    Block& shape = block_of(flames, "shape");
    shape.set("offset", vec3(0.5F, 0.25F, 0.0F));
    shape.set("size", range(0.1F, 0.2F));

    // Nothing worked on, nothing drawn: the stage stays clear.
    CHECK(build_guides(document, GuideOptions{}).guides.empty());
    // A layer worked on with no block focused: only where its things are -
    // its spawn shape, faintly, with no handles - and no other layer's.
    GuideSet idle = build_guides(document, GuideOptions{.layer = flames.uid});
    CHECK(idle.handles.empty() && idle.guides.size() == 1U);
    for (const Guide& guide : idle.guides) {
        CHECK(!guide.focused && guide.layer == flames.uid && document.block(guide.block)->kind == BlockKind::Shape);
    }

    // Focused, the shape carries a handle for its offset and each end of its
    // radius, in metres at the page's scale.
    const GuideSet focused = build_guides(document, GuideOptions{.focus = shape.uid});
    for (const Guide& guide : focused.guides) {
        CHECK(guide.layer == flames.uid);
    }
    const Handle* offset = nullptr;
    const Handle* high = nullptr;
    const Handle* low = nullptr;
    for (const Handle& handle : focused.handles) {
        CHECK(handle.block == shape.uid);
        if (handle.control == "offset") {
            offset = &handle;
        } else if (handle.control == "size") {
            (handle.end == 1 ? high : low) = &handle;
        }
    }
    CHECK(offset != nullptr && high != nullptr && low != nullptr);
    CHECK(offset->at == (ajfx::Vec3{1.0F, 0.5F, 0.0F}));
    CHECK(near(high->at.x - offset->at.x, 0.4F) && near(offset->at.x - low->at.x, 0.2F));

    // Dragging a handle sets its control, back in the page's units.
    CHECK(handle_value(document, *offset, ajfx::Vec3{2.0F, 0.5F, -1.0F}) == vec3(1.0F, 0.25F, -0.5F));
    const std::optional<Value> pulled = handle_value(document, *high, offset->at + ajfx::Vec3{0.0F, 0.0F, 1.0F});
    CHECK(pulled && pulled->x == 0.1F && near(pulled->y, 0.5F));
    // The low end dragged past the high takes it along.
    const std::optional<Value> pushed = handle_value(document, *low, offset->at + ajfx::Vec3{0.8F, 0.0F, 0.0F});
    CHECK(pushed && near(pushed->x, 0.4F) && pushed->y == pushed->x);

    // Wind turns by its heading handle, round the arrow's start.
    std::optional<Block> wind = make_block(document, "wind");
    const Uid wind_uid = insert_block(flames, 0U, std::move(*wind));
    const GuideSet windy = build_guides(document, GuideOptions{.focus = wind_uid});
    CHECK(windy.handles.size() == 1U && windy.handles[0].motion == HandleMotion::Heading);
    const Handle& heading = windy.handles[0];
    CHECK(handle_value(document, heading, heading.origin + ajfx::Vec3{0.0F, 0.0F, -1.0F}) == number(90.0F));

    // Draw in fades by a height above its own centre, wherever that is.
    Block& draw_in = block_of(flames, "draw_in");
    draw_in.set("center", vec3(0.0F, 0.5F, 0.0F));
    std::size_t heights = 0U;
    for (const Handle& handle : build_guides(document, GuideOptions{.focus = draw_in.uid}).handles) {
        if (handle.motion == HandleMotion::Height) {
            ++heights;
            CHECK(near(handle.at.y, (0.5F + number_of(draw_in, "height")) * 2.0F));
            const std::optional<Value> raised = handle_value(document, handle, handle.origin + ajfx::Vec3{0.0F, 3.0F, 0.0F});
            CHECK(raised && near(raised->x, 1.5F));
        }
    }
    CHECK(heights == 1U);

    // A muted layer draws nothing.
    for (Layer& layer : document.layers) {
        layer.enabled = false;
    }
    CHECK(build_guides(document, GuideOptions{.focus = shape.uid}).guides.empty());
}

void the_turbulence_guide_follows_its_field() {
    Document document = recipe("campfire");
    Layer& flames = layer_named(document, "Flames");
    const Uid turbulence = block_of(flames, "turbulence").uid;
    const auto flow = [&](float seconds) {
        for (const Guide& guide : build_guides(document, GuideOptions{.focus = turbulence, .seconds = seconds}).guides) {
            if (guide.block == turbulence) {
                return guide.strokes;
            }
        }
        return std::vector<GuideStroke>{};
    };
    const std::vector<GuideStroke> now = flow(0.0F);
    CHECK(now.size() == 64U && now[0].points.size() == 11U);
    // The field churns: later, the same streamline goes elsewhere.
    const std::vector<GuideStroke> later = flow(2.0F);
    CHECK(later[0].points.front() == now[0].points.front() && !(later[0].points.back() == now[0].points.back()));
    // Only the focused turbulence draws its field.
    for (const Guide& guide : build_guides(document, GuideOptions{.layer = flames.uid}).guides) {
        CHECK(guide.block != turbulence);
    }
}

} // namespace

void add_guides_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"guides_show_the_focused_block_with_handles", guides_show_the_focused_block_with_handles});
    cases.push_back({"the_turbulence_guide_follows_its_field", the_turbulence_guide_follows_its_field});
}
