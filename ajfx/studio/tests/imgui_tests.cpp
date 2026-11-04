#include "imgui_layer.hpp"
#include "scene.hpp"
#include "stage.hpp"
#include "stage_camera.hpp"
#include "stage_gizmo.hpp"
#include "stage_panel.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <optional>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

// Studio's Dear ImGui layer and the panels drawn with it, headless: no window
// and no GPU. ImGui runs on a context of its own with synthetic pointer
// events; its draw data is read as the renderer would be handed it, and the
// panels are clicked where they say they drew their controls.
namespace {

namespace gui = mediaserver::gui;
using namespace ajfx::studio;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"line " + std::to_string(line) + ": check failed: " + expression};
    }
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// An ImGui context with a built font page and no platform behind it.
class Headless final {
public:
    Headless() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{800.0F, 600.0F};
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(static_cast<ImTextureID>(7U));
    }
    ~Headless() { ImGui::DestroyContext(); }
    Headless(const Headless&) = delete;
    Headless& operator=(const Headless&) = delete;

    template <class Draw>
    void frame(Draw&& draw) {
        ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
        ImGui::NewFrame();
        draw();
        ImGui::Render();
    }
    static void move(float x, float y) { ImGui::GetIO().AddMousePosEvent(x, y); }
    static void button(bool down) { ImGui::GetIO().AddMouseButtonEvent(0, down); }
};

void draw_data_reaches_the_draw_list_as_clipped_triangles() {
    Headless imgui;
    imgui.frame([] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2{100.0F, 120.0F}, ImVec2{300.0F, 220.0F},
                                                       IM_COL32(255, 128, 0, 255));
    });
    gui::UiDrawList list;
    append_draw_data(*ImGui::GetDrawData(), list);
    CHECK(!list.batches().empty());
    CHECK(list.vertices().size() % 3U == 0U);
    // Every batch is clipped to the display and samples the font page the
    // context named.
    for (const gui::UiBatch& batch : list.batches()) {
        CHECK(batch.clip_min_x == 0.0F && batch.clip_min_y == 0.0F);
        CHECK(batch.clip_max_x == 800.0F && batch.clip_max_y == 600.0F);
        CHECK(batch.texture == reinterpret_cast<const mediaserver::rhi::Texture*>(std::uintptr_t{7U}));
    }
    // The rectangle's four corners are there in the colour it was given,
    // straight, in pixels.
    bool top_left = false;
    bool bottom_right = false;
    for (const gui::UiVertex& v : list.vertices()) {
        const bool orange = std::abs(v.r - 1.0F) < 0.002F && std::abs(v.g - 128.0F / 255.0F) < 0.002F &&
                            v.b == 0.0F && v.a == 1.0F;
        top_left = top_left || (orange && v.x == 100.0F && v.y == 120.0F);
        bottom_right = bottom_right || (orange && v.x == 300.0F && v.y == 220.0F);
    }
    CHECK(top_left && bottom_right);
}

void draw_data_follows_the_framebuffer_scale() {
    Headless imgui;
    ImGui::GetIO().DisplayFramebufferScale = ImVec2{1.5F, 2.0F};
    imgui.frame([] {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2{10.0F, 20.0F}, ImVec2{30.0F, 40.0F},
                                                       IM_COL32(255, 255, 255, 255));
    });
    gui::UiDrawList list;
    append_draw_data(*ImGui::GetDrawData(), list);
    float most_x = 0.0F;
    float most_y = 0.0F;
    for (const gui::UiVertex& v : list.vertices()) {
        most_x = std::max(most_x, v.x);
        most_y = std::max(most_y, v.y);
    }
    CHECK(most_x == 45.0F && most_y == 80.0F);
    CHECK(list.batches().front().clip_max_x == 1200.0F && list.batches().front().clip_max_y == 1200.0F);
}

// Clicks at the middle of `where` and returns what the panel asked for on any
// of the frames, which is when the button is let go.
StageRequest click(Headless& imgui, StagePanelState& state, const StagePanelIn& in, StageTool& tool, ImVec4 where) {
    const float x = (where.x + where.z) * 0.5F;
    const float y = (where.y + where.w) * 0.5F;
    StageRequest asked = StageRequest::None;
    const auto frame = [&] {
        imgui.frame([&] {
            const StageRequest request = draw_stage_panel(state, in, tool, ImVec2{20.0F, 20.0F});
            if (request != StageRequest::None) {
                asked = request;
            }
        });
    };
    Headless::move(x, y);
    frame();
    Headless::button(true);
    frame();
    Headless::button(false);
    frame();
    frame();
    return asked;
}

ajfx::studio::Document stage_of_three() {
    Document document = new_scene();
    const Uid crate = add_object(document);
    std::get<Object>(document.scene->item(crate)->data).distribution = Distribution::Placed;
    for (int i = 0; i < 3; ++i) {
        add_drop(document.scene->performance, 0U, crate, {static_cast<float>(i), 1.0F, 0.0F}, {}, {});
    }
    add_drop(document.scene->performance, 90U, crate, {0.0F, 3.0F, 0.0F}, {}, {});
    return document;
}

void the_stage_panel_picks_tools_and_asks_for_setup() {
    Headless imgui;
    Document document = stage_of_three();
    StagePanelState state;
    StageTool tool = StageTool::Select;
    const StagePanelIn in{.document = &document, .kind = 0U, .settling = false, .status = {}};
    const auto where = [&](StageControl control) { return state.where[static_cast<std::size_t>(control)]; };
    // One frame to lay it out.
    imgui.frame([&] { static_cast<void>(draw_stage_panel(state, in, tool, ImVec2{20.0F, 20.0F})); });
    imgui.frame([&] { static_cast<void>(draw_stage_panel(state, in, tool, ImVec2{20.0F, 20.0F})); });
    for (std::size_t i = 0U; i < state.where.size(); ++i) {
        const ImVec4 w = state.where[i];
        CHECK(w.z > w.x && w.w > w.y);
    }

    // The tools are picked, and picked again to put back.
    CHECK(click(imgui, state, in, tool, where(StageControl::Place)) == StageRequest::None);
    CHECK(tool == StageTool::Place);
    click(imgui, state, in, tool, where(StageControl::Drop));
    CHECK(tool == StageTool::Drop);
    click(imgui, state, in, tool, where(StageControl::Delete));
    CHECK(tool == StageTool::Delete);
    click(imgui, state, in, tool, where(StageControl::Delete));
    CHECK(tool == StageTool::Select);

    // The setup buttons ask, and the panel changes nothing itself.
    const Performance before = document.scene->performance;
    CHECK(click(imgui, state, in, tool, where(StageControl::Jitter)) == StageRequest::Jitter);
    CHECK(click(imgui, state, in, tool, where(StageControl::Settle)) == StageRequest::Settle);
    CHECK(document.scene->performance == before);
    // Nothing has been moved yet, so there is nothing to put back.
    CHECK(click(imgui, state, in, tool, where(StageControl::Reset)) == StageRequest::None);
    jitter_stage(document.scene->performance, 1U, 0.1F, 3.0F);
    CHECK(click(imgui, state, in, tool, where(StageControl::Reset)) == StageRequest::Reset);

    // While a settle is under way it cannot be asked for again.
    const StagePanelIn busy{.document = &document, .kind = 0U, .settling = true, .status = "Settling"};
    CHECK(click(imgui, state, busy, tool, where(StageControl::Settle)) == StageRequest::None);
}

void a_stage_without_things_cannot_be_settled() {
    Headless imgui;
    Document document = new_scene();
    StagePanelState state;
    StageTool tool = StageTool::Select;
    const StagePanelIn in{.document = &document, .kind = 0U, .settling = false, .status = {}};
    imgui.frame([&] { static_cast<void>(draw_stage_panel(state, in, tool, ImVec2{20.0F, 20.0F})); });
    imgui.frame([&] { static_cast<void>(draw_stage_panel(state, in, tool, ImVec2{20.0F, 20.0F})); });
    CHECK(click(imgui, state, in, tool, state.where[static_cast<std::size_t>(StageControl::Settle)]) ==
          StageRequest::None);
    CHECK(click(imgui, state, in, tool, state.where[static_cast<std::size_t>(StageControl::Jitter)]) ==
          StageRequest::None);
}

// clip = projection * view * (world, 1), column-major, then into the view's
// pixels: worked out here rather than asked of the camera.
ajfx::Vec3 through_matrices(const StageCamera& camera, const gui::Rect& view, ajfx::Vec3 world, float& depth) {
    const std::array<float, 16> v = camera.view_matrix();
    const std::array<float, 16> p = camera.projection_matrix(0.05F, 1000.0F);
    const auto multiply = [](const std::array<float, 16>& m, const float in[4], float out[4]) {
        for (int row = 0; row < 4; ++row) {
            out[row] = m[static_cast<std::size_t>(row)] * in[0] + m[static_cast<std::size_t>(4 + row)] * in[1] +
                       m[static_cast<std::size_t>(8 + row)] * in[2] + m[static_cast<std::size_t>(12 + row)] * in[3];
        }
    };
    const float world4[4]{world.x, world.y, world.z, 1.0F};
    float eye[4]{};
    float clip[4]{};
    multiply(v, world4, eye);
    multiply(p, eye, clip);
    depth = clip[3];
    return ajfx::Vec3{view.x + (clip[0] / clip[3] + 1.0F) * 0.5F * view.width,
                      view.y + (1.0F - clip[1] / clip[3]) * 0.5F * view.height, clip[2] / clip[3]};
}

void the_cameras_matrices_land_where_it_projects() {
    const CameraPose pose{ajfx::Vec3{2.0F, 1.5F, 5.0F}, ajfx::Vec3{0.5F, 0.8F, -0.2F}, 0.12F};
    const gui::Rect view{100.0F, 50.0F, 800.0F, 450.0F};
    const StageCamera camera{pose, view};
    for (const ajfx::Vec3 world : {ajfx::Vec3{0.5F, 0.8F, -0.2F}, ajfx::Vec3{-1.0F, 0.0F, 0.0F}, ajfx::Vec3{2.0F, 3.0F, -4.0F},
                                   ajfx::Vec3{0.0F, 1.0F, 1.0F}}) {
        float depth = 0.0F;
        const ajfx::Vec3 landed = through_matrices(camera, view, world, depth);
        const std::optional<gui::UiPoint> projected = camera.project(world);
        CHECK(projected.has_value() && depth > 0.0F);
        CHECK(std::abs(landed.x - projected->x) < 0.05F && std::abs(landed.y - projected->y) < 0.05F);
        // In front of the camera it is inside the clip range.
        CHECK(landed.z > -1.0F && landed.z < 1.0F);
    }
    // The point the camera looks at is the middle of the view.
    float depth = 0.0F;
    const ajfx::Vec3 middle = through_matrices(camera, view, pose.target, depth);
    CHECK(std::abs(middle.x - 500.0F) < 0.05F && std::abs(middle.y - 275.0F) < 0.05F);
}

// The thing at the origin of a camera looking straight at it, its gizmo
// dragged by the middle across the view to the right.
void dragging_the_gizmos_middle_moves_the_thing_across_the_view() {
    Headless imgui;
    ImGui::GetIO().DisplaySize = ImVec2{1000.0F, 700.0F};
    const gui::Rect view{0.0F, 0.0F, 1000.0F, 700.0F};
    const StageCamera camera{CameraPose{ajfx::Vec3{0.0F, 1.0F, 5.0F}, ajfx::Vec3{0.0F, 1.0F, 0.0F}, 0.11F}, view};
    GizmoPose pose{ajfx::Vec3{0.0F, 1.0F, 0.0F}, ajfx::Vec3{}};
    const std::optional<gui::UiPoint> middle = camera.project(pose.position);
    CHECK(middle.has_value());
    bool took = false;
    std::size_t moves = 0U;
    const auto frame = [&] {
        imgui.frame([&] {
            const GizmoOutcome outcome = draw_gizmo(camera, view, GizmoMode::Move, pose);
            took = took || outcome.took_pointer;
            if (outcome.moved) {
                pose = *outcome.moved;
                ++moves;
            }
        });
    };
    Headless::move(middle->x, middle->y);
    frame();
    frame();
    Headless::button(true);
    frame();
    for (int step = 1; step <= 6; ++step) {
        Headless::move(middle->x + 10.0F * static_cast<float>(step), middle->y);
        frame();
    }
    Headless::button(false);
    frame();
    frame();
    CHECK(took && moves > 0U);
    // Across the view to the right is along +x for this camera, by about the
    // 60 pixels the pointer went (a pixel is 2 * tan(fov/2) * 5 / 700 metres
    // here) and not up or towards the camera.
    const float metres_per_pixel = 2.0F * std::tan(0.11F * 6.28318530718F * 0.5F) * 5.0F / 700.0F;
    CHECK(std::abs(pose.position.x - 60.0F * metres_per_pixel) < 0.05F);
    CHECK(std::abs(pose.position.y - 1.0F) < 0.02F && std::abs(pose.position.z) < 0.02F);
    CHECK(std::abs(pose.turn.x) < 0.01F && std::abs(pose.turn.y) < 0.01F && std::abs(pose.turn.z) < 0.01F);
}

void the_old_interface_is_told_what_imgui_took() {
    gui::InputState input;
    input.set_mouse_position(120.0F, 80.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.add_wheel(2.0F);
    input.add_text("d");
    input.add_key(gui::Key::Delete);
    input.set_move_key(gui::MoveKey::W, true);
    CHECK(input.mouse_pressed(gui::MouseButton::Left) && input.mouse_x() == 120.0F);

    input.withhold_pointer();
    CHECK(input.mouse_x() < -1.0e5F && input.mouse_y() < -1.0e5F);
    CHECK(!input.mouse_down(gui::MouseButton::Left) && !input.mouse_pressed(gui::MouseButton::Left));
    CHECK(input.wheel() == 0.0F);
    // The keyboard is its own: still there until withheld too.
    CHECK(input.text() == "d" && input.keys().size() == 1U && input.move_key_down(gui::MoveKey::W));
    input.withhold_keyboard();
    CHECK(input.text().empty() && input.keys().empty() && !input.move_key_down(gui::MoveKey::W));
}

} // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"draw_data_reaches_the_draw_list_as_clipped_triangles", draw_data_reaches_the_draw_list_as_clipped_triangles},
        {"draw_data_follows_the_framebuffer_scale", draw_data_follows_the_framebuffer_scale},
        {"the_stage_panel_picks_tools_and_asks_for_setup", the_stage_panel_picks_tools_and_asks_for_setup},
        {"a_stage_without_things_cannot_be_settled", a_stage_without_things_cannot_be_settled},
        {"the_cameras_matrices_land_where_it_projects", the_cameras_matrices_land_where_it_projects},
        {"dragging_the_gizmos_middle_moves_the_thing_across_the_view", dragging_the_gizmos_middle_moves_the_thing_across_the_view},
        {"the_old_interface_is_told_what_imgui_took", the_old_interface_is_told_what_imgui_took},
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
