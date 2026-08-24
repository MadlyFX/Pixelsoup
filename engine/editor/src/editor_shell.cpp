#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>

#include <cstdio>
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
    std::optional<render::SceneRenderer> wall;
    SDL_GPUSampler* image_sampler{};
    // Lives across the frame because ImGui defers using the texture id until it
    // renders the draw data later in the same frame.
    SDL_GPUTextureSamplerBinding image_binding{};

    bool sdl_video{};
    bool window_claimed{};
    bool imgui_context{};
    bool imgui_sdl{};
    bool imgui_gpu{};

    // Selection and drag are the editor's own state; they never touch the
    // document. A drag is measured from the position captured at mouse-down, so
    // it stays stable while the committed scene catches up a few frames later.
    doc::NodeId selected{};
    bool has_selection{};
    bool dragging{};
    float drag_start_canvas_x{};
    float drag_start_canvas_y{};
    scene::Vec3 drag_start_position{};

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
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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

    // Render the wall view offscreen, sized to the canvas so canvas pixels and
    // target pixels line up. A submit-and-wait here is fine: an editor is not on
    // the output's frame budget, and it keeps the clip texture ready for ImGui.
    if (impl.device && impl.wall) {
        static_cast<void>(impl.wall->resize(
            *impl.device, scene.canvas.width, scene.canvas.height));
        auto acquired = impl.device->acquire_commands();
        if (acquired) {
            auto commands = std::move(acquired).value();
            static_cast<void>(
                impl.wall->render(commands, scene, scene::whole(scene.canvas)));
            auto submitted = impl.device->submit(std::move(commands));
            if (submitted) {
                static_cast<void>(std::move(submitted).value().wait());
            }
        }
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
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

    ImGui::Begin("Wall");
    if (impl.wall) {
        impl.image_binding = SDL_GPUTextureSamplerBinding{
            static_cast<SDL_GPUTexture*>(impl.wall->target().backend_handle()),
            impl.image_sampler};

        const auto avail = ImGui::GetContentRegionAvail();
        const auto canvas_w = static_cast<float>(scene.canvas.width);
        const auto canvas_h = static_cast<float>(scene.canvas.height);
        ImVec2 display = avail;
        if (avail.x > 0.0F && avail.y > 0.0F && canvas_w > 0.0F
            && canvas_h > 0.0F) {
            const float canvas_aspect = canvas_w / canvas_h;
            const float avail_aspect = avail.x / avail.y;
            display = avail_aspect > canvas_aspect
                ? ImVec2{avail.y * canvas_aspect, avail.y}
                : ImVec2{avail.x, avail.x / canvas_aspect};
        }

        ImGui::Image(
            reinterpret_cast<ImTextureID>(&impl.image_binding), display);

        const auto image_min = ImGui::GetItemRectMin();
        const auto image_size = ImGui::GetItemRectSize();
        const auto mouse = ImGui::GetMousePos();
        const bool have_rect = image_size.x > 0.0F && image_size.y > 0.0F;
        const float canvas_x = have_rect
            ? ((mouse.x - image_min.x) / image_size.x) * canvas_w
            : 0.0F;
        const float canvas_y = have_rect
            ? ((mouse.y - image_min.y) / image_size.y) * canvas_h
            : 0.0F;

        if (ImGui::IsItemHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto* const hit =
                scene::pick_media(scene, scene.canvas, canvas_x, canvas_y);
            if (hit != nullptr) {
                impl.selected = hit->source_node;
                impl.has_selection = true;
                impl.dragging = true;
                impl.drag_start_canvas_x = canvas_x;
                impl.drag_start_canvas_y = canvas_y;
                impl.drag_start_position = scene::translation_of(hit->world);
            }
        }

        if (impl.dragging && have_rect
            && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto delta = scene::screen_drag_to_translation(
                scene.camera,
                scene.canvas,
                impl.drag_start_position,
                canvas_x - impl.drag_start_canvas_x,
                canvas_y - impl.drag_start_canvas_y);
            result.edits.push_back(
                EditIntent{impl.selected, impl.drag_start_position + delta});
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            impl.dragging = false;
        }
    }
    ImGui::End();

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
