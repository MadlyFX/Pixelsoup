#include "test_support.hpp"

// Guides in the preview: what the selected item shows, and what its handles
// set when dragged.
namespace {

using namespace studio_test;

void guides_show_the_selected_item_with_handles() {
    Document document = starter("campfire");
    document.scale = 2.0F;
    const Uid flames = item_named(document, "Flames");
    auto& fire = std::get<ParticleSystem>(document.scene->item(flames)->data);
    fire.position = {0.5F, 0.25F, 0.0F};
    fire.radius = {0.1F, 0.2F};

    // Nothing selected, nothing drawn: the stage stays clear.
    CHECK(build_guides(document, GuideOptions{}).guides.empty());

    // A system: its birth ball with a handle for its position and each end
    // of its radius, in metres at the scene's scale.
    const GuideSet selected = build_guides(document, GuideOptions{.item = flames});
    CHECK(!selected.guides.empty() && selected.guides.front().item == flames && selected.guides.front().focused);
    const Handle* position = nullptr;
    const Handle* high = nullptr;
    const Handle* low = nullptr;
    for (const Handle& handle : selected.handles) {
        CHECK(handle.item == flames);
        if (handle.target == HandleTarget::Position) {
            position = &handle;
        } else if (handle.target == HandleTarget::Radius) {
            (handle.end == 1 ? high : low) = &handle;
        }
    }
    CHECK(position != nullptr && high != nullptr && low != nullptr);
    CHECK(position->at == (ajfx::Vec3{1.0F, 0.5F, 0.0F}));
    CHECK(near(high->at.x - position->at.x, 0.4F) && near(position->at.x - low->at.x, 0.2F));

    // Dragging a handle sets the item, back in the scene's units.
    CHECK(drag_handle(document, *position, ajfx::Vec3{2.0F, 0.5F, -1.0F}));
    CHECK(fire.position == (ajfx::Vec3{1.0F, 0.25F, -0.5F}));
    CHECK(!drag_handle(document, *position, ajfx::Vec3{2.0F, 0.5F, -1.0F})); // already there
    CHECK(drag_handle(document, *high, high->origin + ajfx::Vec3{0.0F, 0.0F, 1.0F}));
    CHECK(fire.radius.min == 0.1F && near(fire.radius.max, 0.5F));
    // The low end dragged past the high takes it along.
    CHECK(drag_handle(document, *low, low->origin + ajfx::Vec3{1.2F, 0.0F, 0.0F}));
    CHECK(near(fire.radius.min, 0.6F) && fire.radius.max == fire.radius.min);

    // Wind turns by its heading handle, round the arrow's start beside the
    // system it acts on.
    const Uid breeze = item_named(document, "Wind");
    const GuideSet windy = build_guides(document, GuideOptions{.item = breeze});
    CHECK(windy.handles.size() == 1U && windy.handles[0].motion == HandleMotion::Heading);
    CHECK(std::any_of(windy.guides.begin(), windy.guides.end(),
                      [&](const Guide& g) { return g.item == item_named(document, "Smoke") && !g.focused; }));
    CHECK(drag_handle(document, windy.handles[0], windy.handles[0].origin + ajfx::Vec3{0.0F, 0.0F, -1.0F}));
    CHECK(std::get<Force>(document.scene->item(breeze)->data).heading == 90.0F);

    // A draw-in fades by a height above its own centre, wherever that is.
    const Uid funnel = add_force(document, ForceKind::DrawIn);
    auto& draw_in = std::get<Force>(document.scene->item(funnel)->data);
    draw_in.position = {0.0F, 0.5F, 0.0F};
    draw_in.height = 1.0F;
    std::size_t heights = 0U;
    for (const Handle& handle : build_guides(document, GuideOptions{.item = funnel}).handles) {
        if (handle.motion == HandleMotion::Height) {
            ++heights;
            CHECK(near(handle.at.y, (0.5F + 1.0F) * 2.0F));
            CHECK(drag_handle(document, handle, handle.origin + ajfx::Vec3{0.0F, 3.0F, 0.0F}));
            CHECK(near(std::get<Force>(document.scene->item(funnel)->data).height, 1.5F));
        }
    }
    CHECK(heights == 1U);

    // A switched-off item draws nothing.
    document.scene->item(flames)->simulate = false;
    CHECK(build_guides(document, GuideOptions{.item = flames}).guides.empty());
}

void an_object_shows_what_it_collides_as() {
    Document document = starter("wall");
    const Uid ball = item_named(document, "Ball");
    auto& thrown = std::get<Object>(document.scene->item(ball)->data);
    thrown.collider = ObjectCollider::Sphere;
    const GuideSet guides = build_guides(document, GuideOptions{.item = ball});
    CHECK(guides.guides.size() == 1U && guides.guides.front().kind == GuideKind::Collider);
    // A marker (no outline is read yet) and its ball, faintly.
    CHECK(std::any_of(guides.guides.front().strokes.begin(), guides.guides.front().strokes.end(),
                      [](const GuideStroke& s) { return s.faint && s.closed; }));
    // Its radius handle sets its size.
    const auto radius = std::find_if(guides.handles.begin(), guides.handles.end(),
                                     [](const Handle& h) { return h.target == HandleTarget::Radius; });
    CHECK(radius != guides.handles.end());
    CHECK(drag_handle(document, *radius, radius->origin + ajfx::Vec3{0.5F, 0.0F, 0.0F}));
    CHECK(near(std::get<Object>(document.scene->item(ball)->data).size, 1.0F));

    // A system shows the Static things its particles meet, without handles.
    Document room = starter("smoke_room");
    const Uid fire = item_named(room, "Fire");
    const GuideSet beside = build_guides(room, GuideOptions{.item = fire});
    const std::size_t met = static_cast<std::size_t>(std::count_if(
        beside.guides.begin(), beside.guides.end(), [](const Guide& g) { return g.kind == GuideKind::Collider; }));
    CHECK(met == 2U);
    for (const Handle& handle : beside.handles) {
        CHECK(handle.item == fire);
    }
}

} // namespace

void add_guides_tests(std::vector<studio_test::Case>& cases) {
    cases.push_back({"guides show the selected item with handles", guides_show_the_selected_item_with_handles});
    cases.push_back({"an object shows what it collides as", an_object_shows_what_it_collides_as});
}
