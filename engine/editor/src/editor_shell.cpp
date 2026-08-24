#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include <SDL3/SDL.h>

// Dear ImGui and its backends are not clean under this project's
// warnings-as-errors, so their headers are included with warnings silenced. The
// editor's own code below stays fully checked.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <ImGuizmo.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace mediaserver::editor {
namespace {

namespace rhi = mediaserver::rhi;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

using Result = core::Result<std::unique_ptr<EditorShell>, EditorError>;

} // namespace

bool editor_available() noexcept {
    return render::SceneRenderer::shaders_available() && rhi::Device::available();
}

struct EditorShell::Impl final {
    SDL_Window* window{};
    std::optional<rhi::Device> device;
    SDL_GPUDevice* gpu{}; // borrowed from device
    // Two editors of the one scene: the wall view through the show camera, and a
    // free 3D view. The show camera is the scene's; the 3D view's is the editor
    // camera below.
    std::optional<render::SceneRenderer> wall;
    std::optional<render::SceneRenderer> view3d;
    SDL_GPUSampler* image_sampler{};
    // Live across the frame because ImGui defers using the texture ids until it
    // renders the draw data later in the same frame.
    SDL_GPUTextureSamplerBinding wall_binding{};
    SDL_GPUTextureSamplerBinding view3d_binding{};

    bool sdl_video{};
    bool window_claimed{};
    bool imgui_context{};
    bool imgui_sdl{};
    bool imgui_gpu{};

    // The 3D view's camera: an orbit around a target, all editor-local state
    // that never enters the document. Angles are in turns.
    float cam_yaw{0.10F};
    float cam_pitch{0.06F};
    float cam_distance{9.0F};
    scene::Vec3 cam_target{0.0F, 0.0F, 0.0F};

    // Selection and drag are the editor's own state; they never touch the
    // document. A drag is measured from the position, camera, and canvas
    // captured at mouse-down, so it stays stable whichever view it began in and
    // while the committed scene catches up a few frames later.
    doc::NodeId selected{};
    bool has_selection{};
    bool dragging_clip{};
    bool orbiting{};
    float drag_start_canvas_x{};
    float drag_start_canvas_y{};
    scene::Vec3 drag_start_position{};
    scene::Camera drag_camera{};
    scene::Canvas drag_canvas{};

    ~Impl() {
        if (imgui_gpu) {
            ImGui_ImplSDLGPU3_Shutdown();
        }
        if (imgui_sdl) {
            ImGui_ImplSDL3_Shutdown();
        }
        if (imgui_context) {
            ImGui::DestroyContext();
        }
        view3d.reset();
        wall.reset();
        if (gpu != nullptr && image_sampler != nullptr) {
            SDL_ReleaseGPUSampler(gpu, image_sampler);
        }
        if (gpu != nullptr && window_claimed && window != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(gpu, window);
        }
        device.reset();
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        if (sdl_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }
};

EditorShell::EditorShell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

EditorShell::~EditorShell() = default;

Result EditorShell::create(const EditorConfig& config) noexcept {
    if (!editor_available()) {
        return Result::failure(EditorError::Unavailable);
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return Result::failure(EditorError::DisplayUnavailable);
    }

    auto* raw = new (std::nothrow) Impl{};
    if (raw == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return Result::failure(EditorError::AllocationFailed);
    }
    std::unique_ptr<Impl> impl{raw};
    impl->sdl_video = true;

    impl->window = SDL_CreateWindow(
        config.title.c_str(),
        static_cast<int>(config.width),
        static_cast<int>(config.height),
        SDL_WINDOW_RESIZABLE);
    if (impl->window == nullptr) {
        return Result::failure(EditorError::DisplayUnavailable);
    }
    if (config.position_x >= 0 && config.position_y >= 0) {
        static_cast<void>(SDL_SetWindowPosition(
            impl->window, config.position_x, config.position_y));
    }

    auto device = rhi::Device::create(rhi::DeviceConfig{});
    if (!device) {
        return Result::failure(EditorError::Unavailable);
    }
    impl->device.emplace(std::move(device).value());
    impl->gpu = static_cast<SDL_GPUDevice*>(impl->device->backend_handle());

    if (!SDL_ClaimWindowForGPUDevice(impl->gpu, impl->window)) {
        return Result::failure(EditorError::DisplayUnavailable);
    }
    impl->window_claimed = true;

    // The wall view renders at a fixed preview resolution; it is resized to the
    // scene's canvas each frame so a mouse pixel maps to a canvas pixel.
    auto wall = render::SceneRenderer::create(
        *impl->device, config.width, config.height);
    if (!wall) {
        return Result::failure(EditorError::Unavailable);
    }
    impl->wall.emplace(std::move(wall).value());

    auto view3d = render::SceneRenderer::create(
        *impl->device, config.width, config.height);
    if (!view3d) {
        return Result::failure(EditorError::Unavailable);
    }
    impl->view3d.emplace(std::move(view3d).value());

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    impl->image_sampler = SDL_CreateGPUSampler(impl->gpu, &sampler_info);
    if (impl->image_sampler == nullptr) {
        return Result::failure(EditorError::AllocationFailed);
    }

    IMGUI_CHECKVERSION();
    if (ImGui::CreateContext() == nullptr) {
        return Result::failure(EditorError::AllocationFailed);
    }
    impl->imgui_context = true;
    // ImGuizmo is a separate translation unit with its own context pointer; point
    // it at the one context we just created so its overlay shares our input.
    ImGuizmo::SetImGuiContext(ImGui::GetCurrentContext());
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Move windows only by their title bar, so a drag inside the wall viewport
    // manipulates the clip rather than sliding the panel around.
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForSDLGPU(impl->window)) {
        return Result::failure(EditorError::DisplayUnavailable);
    }
    impl->imgui_sdl = true;

    ImGui_ImplSDLGPU3_InitInfo init_info{};
    init_info.Device = impl->gpu;
    init_info.ColorTargetFormat =
        SDL_GetGPUSwapchainTextureFormat(impl->gpu, impl->window);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&init_info)) {
        return Result::failure(EditorError::DisplayUnavailable);
    }
    impl->imgui_gpu = true;

    auto* const shell = new (std::nothrow) EditorShell{std::move(impl)};
    if (shell == nullptr) {
        // Allocation failed before the move happened, so `impl` is still valid
        // and its destructor unwinds everything set up above.
        return Result::failure(EditorError::AllocationFailed);
    }
    return Result::success(std::unique_ptr<EditorShell>{shell});
}

EditorFrame EditorShell::frame(const scene::Scene& scene) noexcept {
    auto& impl = *impl_;
    EditorFrame result{};

    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            result.status = EditorStatus::Closed;
        } else if (
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            && event.window.windowID == SDL_GetWindowID(impl.window)) {
            result.status = EditorStatus::Closed;
        }
    }

    // The 3D view's camera: an orbit derived from the editor's own state. It
    // uses the scene's canvas, so both views map a pixel to the same coordinate
    // space and the manipulation math is identical.
    scene::Camera editor_camera{};
    editor_camera.target = impl.cam_target;
    {
        const float cp = scene::deterministic::cos_turns(impl.cam_pitch);
        const float sp = scene::deterministic::sin_turns(impl.cam_pitch);
        const float cy = scene::deterministic::cos_turns(impl.cam_yaw);
        const float sy = scene::deterministic::sin_turns(impl.cam_yaw);
        editor_camera.eye = impl.cam_target
            + scene::Vec3{
                impl.cam_distance * cp * sy,
                impl.cam_distance * sp,
                impl.cam_distance * cp * cy};
        editor_camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
        editor_camera.field_of_view = 0.14F;
        editor_camera.near_plane = 0.05F;
        editor_camera.far_plane = 400.0F;
    }
    // The one scene, seen from the editor camera. A copy so the wall keeps the
    // show camera; the vectors are small at this stage.
    scene::Scene scene_3d = scene;
    scene_3d.camera = editor_camera;

    // Render both viewports offscreen, each sized to the canvas so canvas pixels
    // and target pixels line up. A submit-and-wait is fine here: an editor is
    // not on the output's frame budget.
    if (impl.device && impl.wall && impl.view3d) {
        static_cast<void>(impl.wall->resize(
            *impl.device, scene.canvas.width, scene.canvas.height));
        static_cast<void>(impl.view3d->resize(
            *impl.device, scene.canvas.width, scene.canvas.height));
        auto acquired = impl.device->acquire_commands();
        if (acquired) {
            auto commands = std::move(acquired).value();
            static_cast<void>(
                impl.wall->render(commands, scene, scene::whole(scene.canvas)));
            static_cast<void>(impl.view3d->render(
                commands, scene_3d, scene::whole(scene_3d.canvas)));
            auto submitted = impl.device->submit(std::move(commands));
            if (submitted) {
                static_cast<void>(std::move(submitted).value().wait());
            }
        }
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    ImGui::DockSpaceOverViewport(0U, ImGui::GetMainViewport());

    ImGui::Begin("Elements");
    for (const auto& clip : scene.media) {
        char label[64]{};
        std::snprintf(
            label,
            sizeof(label),
            "clip  node %llu",
            static_cast<unsigned long long>(clip.source_node.value()));
        const bool is_selected =
            impl.has_selection && impl.selected == clip.source_node;
        if (ImGui::Selectable(label, is_selected)) {
            impl.selected = clip.source_node;
            impl.has_selection = true;
        }
    }
    if (scene.media.empty()) {
        ImGui::TextUnformatted("no clips in the scene");
    }
    ImGui::End();

    // One viewport image: an InvisibleButton captures the mouse (so a drag moves
    // the clip, not the window) with the rendered view drawn into its rect.
    // Returns the interaction state and the canvas coordinate under the cursor.
    struct Viewport final {
        bool hovered{};
        bool active{};
        bool have_rect{};
        float canvas_x{};
        float canvas_y{};
        // The image's rectangle on screen, for overlays like the gizmo.
        float origin_x{};
        float origin_y{};
        float width{};
        float height{};
    };
    // An optional overlay runs inside the viewport window after the image is
    // drawn; it returns true when it owns the cursor (e.g. the gizmo is hovered
    // or being dragged), which suppresses the capture button below.
    const auto draw_viewport =
        [&](const char* id,
            SDL_GPUTextureSamplerBinding& binding,
            render::SceneRenderer& renderer,
            scene::Canvas canvas,
            const std::function<bool(const Viewport&)>& overlay = {}) -> Viewport {
        binding = SDL_GPUTextureSamplerBinding{
            static_cast<SDL_GPUTexture*>(renderer.target().backend_handle()),
            impl.image_sampler};
        const auto avail = ImGui::GetContentRegionAvail();
        const auto cw = static_cast<float>(canvas.width);
        const auto ch = static_cast<float>(canvas.height);
        ImVec2 display = avail;
        if (avail.x > 0.0F && avail.y > 0.0F && cw > 0.0F && ch > 0.0F) {
            const float ca = cw / ch;
            const float aa = avail.x / avail.y;
            display = aa > ca ? ImVec2{avail.y * ca, avail.y}
                              : ImVec2{avail.x, avail.x / ca};
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        Viewport vp{};
        vp.have_rect = display.x > 0.0F && display.y > 0.0F;
        vp.origin_x = origin.x;
        vp.origin_y = origin.y;
        vp.width = display.x;
        vp.height = display.y;
        if (vp.have_rect) {
            // The rendered view first, so an overlay (the gizmo) draws over it.
            ImGui::GetWindowDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(&binding),
                origin,
                ImVec2{origin.x + display.x, origin.y + display.y});
            // The overlay runs before the capture button and reports whether it
            // owns the cursor. When it does, the button is skipped entirely:
            // ImGuizmo will not begin a drag while ImGui::IsAnyItemHovered is
            // true, and that check spans this frame AND the previous one, so a
            // full-viewport button sitting under the gizmo blocks it every frame
            // the cursor rests there - reordering is not enough, the button has
            // to be absent. Window-move is already restricted to the title bar,
            // so dropping it while the gizmo is under the cursor is safe.
            bool overlay_owns_cursor = false;
            if (overlay) {
                overlay_owns_cursor = overlay(vp);
            }
            if (!overlay_owns_cursor) {
                ImGui::InvisibleButton(
                    id, display, ImGuiButtonFlags_MouseButtonLeft);
                vp.hovered = ImGui::IsItemHovered();
                vp.active = ImGui::IsItemActive();
                const auto mouse = ImGui::GetMousePos();
                vp.canvas_x = ((mouse.x - origin.x) / display.x) * cw;
                vp.canvas_y = ((mouse.y - origin.y) / display.y) * ch;
            }
        }
        return vp;
    };

    // Clip pick and drag, shared by both views. The drag captures the view's
    // camera and canvas at mouse-down, so a move stays consistent whichever view
    // it began in and while the committed scene lags a few frames behind.
    const auto handle_clip =
        [&](const scene::Scene& view_scene, scene::Canvas canvas, const Viewport& vp) {
            if (vp.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const auto* const hit = scene::pick_media(
                    view_scene, canvas, vp.canvas_x, vp.canvas_y);
                if (hit != nullptr) {
                    impl.selected = hit->source_node;
                    impl.has_selection = true;
                    impl.dragging_clip = true;
                    impl.drag_start_canvas_x = vp.canvas_x;
                    impl.drag_start_canvas_y = vp.canvas_y;
                    impl.drag_start_position = scene::translation_of(hit->world);
                    impl.drag_camera = view_scene.camera;
                    impl.drag_canvas = canvas;
                }
            }
            if (impl.dragging_clip && vp.active) {
                const auto delta = scene::screen_drag_to_translation(
                    impl.drag_camera,
                    impl.drag_canvas,
                    impl.drag_start_position,
                    vp.canvas_x - impl.drag_start_canvas_x,
                    vp.canvas_y - impl.drag_start_canvas_y);
                result.edits.push_back(
                    EditIntent{impl.selected, impl.drag_start_position + delta});
            }
        };

    ImGui::Begin("Wall");
    if (impl.wall) {
        const auto vp =
            draw_viewport("wall_canvas", impl.wall_binding, *impl.wall, scene.canvas);
        handle_clip(scene, scene.canvas, vp);
    }
    ImGui::End();

    ImGui::Begin("3D View");
    if (impl.view3d) {
        // An axis-constrained translate gizmo on the selected clip, drawn as the
        // viewport's overlay so it takes the click before the capture button. It
        // is an overlay (never depth-tested), so it uses a standard 0..1 depth
        // projection rather than the renderer's reversed one; both share the
        // editor camera's view, so the handles land on the clip. Dragging a
        // handle authors the same EditIntent a bare drag does, so an edit here
        // moves the clip on the wall and the node exactly the same way.
        bool gizmo_capturing = false;
        const auto gizmo_overlay = [&](const Viewport& vp) -> bool {
            if (!impl.has_selection) {
                return false;
            }
            const auto* const clip = scene::media_for(scene_3d, impl.selected);
            if (clip == nullptr) {
                return false;
            }
            const auto vm = scene::view_for_slice(
                editor_camera, scene_3d.canvas, scene::whole(scene_3d.canvas));
            std::array<float, 16U> view_m = vm.view.m;
            std::array<float, 16U> proj_m = vm.projection.m;
            const float near_p = editor_camera.near_plane;
            const float far_p = editor_camera.far_plane;
            proj_m[10] = far_p / (near_p - far_p);
            proj_m[14] = (near_p * far_p) / (near_p - far_p);
            std::array<float, 16U> model_m = clip->world.m;

            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(vp.origin_x, vp.origin_y, vp.width, vp.height);
            ImGuizmo::Manipulate(
                view_m.data(),
                proj_m.data(),
                ImGuizmo::TRANSLATE,
                ImGuizmo::WORLD,
                model_m.data());
            if (ImGuizmo::IsUsing()) {
                // Column-major: the translation column is elements 12..14.
                result.edits.push_back(EditIntent{
                    impl.selected,
                    scene::Vec3{model_m[12], model_m[13], model_m[14]}});
            }
            gizmo_capturing = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
            return gizmo_capturing;
        };

        const auto vp = draw_viewport(
            "view3d_canvas",
            impl.view3d_binding,
            *impl.view3d,
            scene_3d.canvas,
            gizmo_overlay);

        // The gizmo has priority: only with the cursor off it does a bare drag
        // (move the clip) or an orbit/dolly of the editor camera run. Both are
        // editor-local and never touch the document.
        if (!gizmo_capturing) {
            handle_clip(scene_3d, scene_3d.canvas, vp);
            if (vp.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !impl.dragging_clip) {
                impl.orbiting = true;
            }
            if (impl.orbiting && vp.active) {
                const auto move = ImGui::GetIO().MouseDelta;
                impl.cam_yaw -= move.x * 0.0015F;
                impl.cam_pitch =
                    std::clamp(impl.cam_pitch + move.y * 0.0015F, -0.24F, 0.24F);
            }
            if (vp.hovered) {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.0F) {
                    impl.cam_distance = std::clamp(
                        impl.cam_distance * (1.0F - wheel * 0.1F), 1.0F, 200.0F);
                }
            }
        }
    }
    ImGui::End();

    // A drag or orbit ends when the button is released, whichever view owns it.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        impl.dragging_clip = false;
        impl.orbiting = false;
    }

    ImGui::Render();
    ImDrawData* const draw_data = ImGui::GetDrawData();
    const bool minimized = draw_data->DisplaySize.x <= 0.0F
        || draw_data->DisplaySize.y <= 0.0F;

    SDL_GPUCommandBuffer* const command_buffer =
        SDL_AcquireGPUCommandBuffer(impl.gpu);
    if (command_buffer != nullptr) {
        SDL_GPUTexture* swapchain = nullptr;
        const bool acquired = SDL_WaitAndAcquireGPUSwapchainTexture(
            command_buffer, impl.window, &swapchain, nullptr, nullptr);
        if (acquired && swapchain != nullptr && !minimized) {
            Imgui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
            SDL_GPUColorTargetInfo target{};
            target.texture = swapchain;
            target.clear_color = SDL_FColor{0.10F, 0.10F, 0.12F, 1.0F};
            target.load_op = SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* const pass =
                SDL_BeginGPURenderPass(command_buffer, &target, 1U, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, pass);
            SDL_EndGPURenderPass(pass);
        }
        static_cast<void>(SDL_SubmitGPUCommandBuffer(command_buffer));
    }

    return result;
}

} // namespace mediaserver::editor
