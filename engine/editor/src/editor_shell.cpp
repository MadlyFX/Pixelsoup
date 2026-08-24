#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <set>
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
#include <imgui_internal.h>
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

// A compact, dark theme in the After Effects register: near-black panels, tight
// padding, small rounding, and a muted blue accent. Applied over StyleColorsDark
// so anything not set keeps a sensible default.
void apply_editor_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0F;
    style.FrameRounding = 3.0F;
    style.GrabRounding = 3.0F;
    style.TabRounding = 3.0F;
    style.ScrollbarRounding = 3.0F;
    style.WindowPadding = ImVec2{8.0F, 6.0F};
    style.FramePadding = ImVec2{6.0F, 3.0F};
    style.ItemSpacing = ImVec2{6.0F, 4.0F};
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    const auto rgba = [](int r, int g, int b, int a) {
        return ImVec4{
            static_cast<float>(r) / 255.0F,
            static_cast<float>(g) / 255.0F,
            static_cast<float>(b) / 255.0F,
            static_cast<float>(a) / 255.0F};
    };
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = rgba(24, 24, 28, 255);
    c[ImGuiCol_ChildBg] = rgba(20, 20, 24, 255);
    c[ImGuiCol_PopupBg] = rgba(28, 28, 34, 255);
    c[ImGuiCol_Border] = rgba(12, 12, 15, 255);
    c[ImGuiCol_FrameBg] = rgba(38, 38, 45, 255);
    c[ImGuiCol_FrameBgHovered] = rgba(52, 52, 62, 255);
    c[ImGuiCol_FrameBgActive] = rgba(60, 60, 72, 255);
    c[ImGuiCol_TitleBg] = rgba(18, 18, 22, 255);
    c[ImGuiCol_TitleBgActive] = rgba(30, 30, 38, 255);
    c[ImGuiCol_Header] = rgba(48, 60, 84, 255);
    c[ImGuiCol_HeaderHovered] = rgba(58, 72, 100, 255);
    c[ImGuiCol_HeaderActive] = rgba(66, 82, 112, 255);
    c[ImGuiCol_Button] = rgba(46, 52, 66, 255);
    c[ImGuiCol_ButtonHovered] = rgba(60, 70, 92, 255);
    c[ImGuiCol_ButtonActive] = rgba(74, 88, 116, 255);
    c[ImGuiCol_Tab] = rgba(28, 28, 34, 255);
    c[ImGuiCol_TabHovered] = rgba(58, 72, 100, 255);
    c[ImGuiCol_TabActive] = rgba(44, 54, 74, 255);
    c[ImGuiCol_TabUnfocused] = rgba(24, 24, 28, 255);
    c[ImGuiCol_TabUnfocusedActive] = rgba(34, 40, 52, 255);
    c[ImGuiCol_SliderGrab] = rgba(120, 150, 210, 255);
    c[ImGuiCol_SliderGrabActive] = rgba(150, 180, 240, 255);
    c[ImGuiCol_CheckMark] = rgba(150, 180, 240, 255);
    c[ImGuiCol_Separator] = rgba(12, 12, 15, 255);
    c[ImGuiCol_Text] = rgba(220, 220, 228, 255);
    c[ImGuiCol_TextDisabled] = rgba(120, 120, 132, 255);
}

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
    // document. Selection is a set of clips; `primary` is the anchor - the clip
    // the gizmo sits on and whose depth a bare drag uses. A drag captures every
    // selected clip's position (and the camera and canvas) at mouse-down, so the
    // whole group moves by one world delta and stays stable whichever view it
    // began in and while the committed scene catches up a few frames later.
    std::vector<doc::NodeId> selection;
    doc::NodeId primary{};
    bool dragging_clip{};
    bool orbiting{};
    float drag_start_canvas_x{};
    float drag_start_canvas_y{};
    scene::Camera drag_camera{};
    scene::Canvas drag_canvas{};
    // Captured at gesture start (parallel arrays): each selected clip and its
    // start position, plus the primary's start - the drag's depth and the gizmo's
    // base. The group moves rigidly: new_position[i] = group_start[i] + delta.
    std::vector<doc::NodeId> group_nodes;
    std::vector<scene::Vec3> group_start;
    scene::Vec3 primary_start{};

    [[nodiscard]] bool has_selection() const noexcept { return !selection.empty(); }
    [[nodiscard]] bool is_selected(doc::NodeId node) const noexcept {
        return std::find(selection.begin(), selection.end(), node) != selection.end();
    }
    void select_only(doc::NodeId node) {
        selection.assign(1U, node);
        primary = node;
    }
    void toggle(doc::NodeId node) {
        const auto it = std::find(selection.begin(), selection.end(), node);
        if (it != selection.end()) {
            selection.erase(it);
        } else {
            selection.push_back(node);
        }
        primary = node;
    }
    // The anchor for the gizmo and the drag depth: the primary if it is still
    // selected, otherwise the first selection member. Only call with a selection.
    [[nodiscard]] doc::NodeId anchor() const noexcept {
        return is_selected(primary) ? primary : selection.front();
    }

    // A continuous manipulation shares one gesture id so the Director groups a
    // whole drag into one undo step. `manipulating` is true from the first edit
    // of a gesture until the mouse is released; the counter only ever climbs, so
    // ids stay unique within the run's scene-edit undo context.
    core::u64 edit_gesture{};
    core::u64 next_edit_gesture{1U};
    bool manipulating{};

    // AE-style timeline view state, all editor-local: the horizontal zoom
    // (pixels per frame), the leftmost frame shown, and which layer rows and
    // transform groups are twirled open. Group key is node*4 + group index.
    float tl_px_per_frame{8.0F};
    float tl_scroll_frames{0.0F};
    std::set<core::u64> expanded_layers;
    std::set<core::u64> expanded_groups;

    // Built once, so the panels open in the After Effects arrangement the first
    // time; after that the operator's own docking (saved to imgui.ini) wins.
    bool first_layout{true};

    // Dragging a keyframe diamond to retime it. The move is authored once on
    // release (from the original frame to where it was dropped), so the lagging
    // committed document does not fight the drag.
    bool dragging_key{};
    doc::NodeId drag_key_target{};
    scene::TransformChannel drag_key_component{};
    core::i64 drag_key_from{};
    core::i64 drag_key_to{};

    // Starts a gesture if one is not already running and returns its id. Called
    // the moment a drag or gizmo manipulation first produces an edit.
    core::u64 begin_gesture() noexcept {
        if (!manipulating) {
            manipulating = true;
            edit_gesture = next_edit_gesture++;
        }
        return edit_gesture;
    }

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
    apply_editor_theme();

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

namespace {

// The transform-component groups a layer twirls open into, and the channel
// index each maps to (group * 3 + component), matching scene::TransformChannel.
constexpr std::array<const char*, 3U> group_names{{"Position", "Rotation", "Scale"}};
constexpr std::array<std::array<const char*, 3U>, 3U> component_names{{
    {{"X", "Y", "Z"}},
    {{"Yaw", "Pitch", "Roll"}},
    {{"X", "Y", "Z"}},
}};

// A filled diamond centred at (cx, cy) - the keyframe marker.
void draw_diamond(ImDrawList* draw, float cx, float cy, float r, ImU32 color) {
    const ImVec2 points[4]{
        {cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy}};
    draw->AddConvexPolyFilled(points, 4, color);
}

// The value of one transform component in an element's authored transform - what
// a key on that component captures.
[[nodiscard]] double element_value(
    const ElementInfo& element, scene::TransformChannel component) {
    switch (component) {
    case scene::TransformChannel::PositionX:
        return element.position.x;
    case scene::TransformChannel::PositionY:
        return element.position.y;
    case scene::TransformChannel::PositionZ:
        return element.position.z;
    case scene::TransformChannel::RotationYaw:
        return element.rotation.x;
    case scene::TransformChannel::RotationPitch:
        return element.rotation.y;
    case scene::TransformChannel::RotationRoll:
        return element.rotation.z;
    case scene::TransformChannel::ScaleX:
        return element.scale.x;
    case scene::TransformChannel::ScaleY:
        return element.scale.y;
    case scene::TransformChannel::ScaleZ:
        return element.scale.z;
    case scene::TransformChannel::Count:
        break;
    }
    return 0.0;
}

} // namespace

void EditorShell::draw_timeline(
    Impl& impl,
    EditorFrame& result,
    const scene::Scene& scene,
    const EditorTimeline& timeline,
    const std::vector<scene::ChannelKeyframes>& keyframes,
    const std::vector<ElementInfo>& elements) noexcept {
    // The timeline: an After Effects-style layer list beside a time graph. The
    // left lists scene elements (twirl to Position/Rotation/Scale, and again to
    // the components); the right shows a frame ruler, a playhead across every
    // lane, layer bars, and a diamond per keyframe. Time zooms and scrolls
    // independently of the vertical row scroll.
    ImGui::Begin("Timeline");

    const auto first = static_cast<core::i64>(timeline.first_frame);
    const auto last = static_cast<core::i64>(
        std::max(timeline.last_frame, timeline.first_frame));

    // Layers: clips first, then objects.
    struct Layer final {
        doc::NodeId node;
        std::string name;
        ImU32 color;
    };
    std::vector<Layer> layers;
    for (const auto& clip : scene.media) {
        char name[48]{};
        std::snprintf(
            name,
            sizeof(name),
            "clip %llu",
            static_cast<unsigned long long>(clip.source_node.value()));
        layers.push_back(Layer{clip.source_node, name, IM_COL32(96, 165, 250, 255)});
    }
    for (const auto& object : scene.instances) {
        char name[48]{};
        std::snprintf(
            name,
            sizeof(name),
            "object %llu",
            static_cast<unsigned long long>(object.source.value()));
        layers.push_back(Layer{object.source, name, IM_COL32(160, 160, 172, 255)});
    }

    const auto frames_for =
        [&keyframes](doc::NodeId node, scene::TransformChannel channel)
        -> const std::vector<core::i64>* {
        for (const auto& entry : keyframes) {
            if (entry.target == node && entry.component == channel) {
                return &entry.frames;
            }
        }
        return nullptr;
    };

    // The selection anchor's authored transform, for keying at the playhead.
    const ElementInfo* anchor_info = nullptr;
    if (impl.has_selection()) {
        for (const auto& element : elements) {
            if (element.node == impl.anchor()) {
                anchor_info = &element;
                break;
            }
        }
    }
    const auto key_group = [&](int group) {
        if (anchor_info == nullptr) {
            return;
        }
        const auto gesture = impl.next_edit_gesture++;
        for (int c = 0; c < 3; ++c) {
            const auto component =
                static_cast<scene::TransformChannel>(group * 3 + c);
            result.keys.push_back(KeyIntent{
                impl.anchor(),
                component,
                timeline.show_frame,
                element_value(*anchor_info, component),
                false,
                gesture});
        }
    };
    const auto clear_transform = [&]() {
        if (!impl.has_selection()) {
            return;
        }
        const auto gesture = impl.next_edit_gesture++;
        for (int i = 0; i < 9; ++i) {
            result.keys.push_back(KeyIntent{
                impl.anchor(),
                static_cast<scene::TransformChannel>(i),
                0U,
                0.0,
                true,
                gesture});
        }
    };

    // Toolbar: transport, current frame, transport state, and keying.
    if (ImGui::SmallButton("|<")) {
        result.transport = TransportRequest::GoToStart;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) {
        result.transport = TransportRequest::JumpBack;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("<")) {
        result.transport = TransportRequest::StepBack;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(timeline.playing ? "Pause" : "Play")) {
        result.transport = TransportRequest::TogglePlay;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">")) {
        result.transport = TransportRequest::StepForward;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) {
        result.transport = TransportRequest::JumpForward;
    }
    ImGui::SameLine();
    ImGui::Text(
        "  frame %lld / %lld",
        static_cast<long long>(timeline.show_frame),
        static_cast<long long>(last));
    ImGui::SameLine();
    ImGui::TextDisabled(timeline.playing ? "  PLAYING" : "  PAUSED");
    if (impl.has_selection()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Key P")) {
            key_group(0);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Key R")) {
            key_group(1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Key S")) {
            key_group(2);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            clear_transform();
        }
    }

    // Layout metrics.
    constexpr float row_h = 22.0F;
    constexpr float ruler_h = 22.0F;
    constexpr float list_w = 236.0F;
    constexpr float indent_w = 14.0F;
    const float px_per_frame = impl.tl_px_per_frame;

    const ImVec2 body = ImGui::GetCursorScreenPos();
    const float avail_w =
        std::max(ImGui::GetContentRegionAvail().x, list_w + 80.0F);
    const float avail_h =
        std::max(ImGui::GetContentRegionAvail().y, ruler_h + row_h);
    const float graph_x0 = body.x + list_w;
    const float right = body.x + avail_w;
    auto* const draw = ImGui::GetWindowDrawList();

    const auto frame_to_x = [&](double frame) -> float {
        return graph_x0
            + static_cast<float>((frame - impl.tl_scroll_frames) * px_per_frame);
    };
    const auto x_to_frame = [&](float x) -> double {
        return impl.tl_scroll_frames
            + static_cast<double>(x - graph_x0) / px_per_frame;
    };

    // While a diamond is being dragged, follow the cursor (clamped to the range);
    // the move is authored on release so the lagging document does not fight it.
    if (impl.dragging_key && px_per_frame > 0.0F) {
        impl.drag_key_to = static_cast<core::i64>(
            std::clamp(
                x_to_frame(ImGui::GetMousePos().x),
                static_cast<double>(first),
                static_cast<double>(last))
            + 0.5);
    }

    // Ruler background and frame labels.
    draw->AddRectFilled(
        body, ImVec2{right, body.y + ruler_h}, IM_COL32(34, 34, 40, 255));
    draw->AddRectFilled(
        body, ImVec2{graph_x0, body.y + ruler_h}, IM_COL32(24, 24, 30, 255));
    draw->PushClipRect(
        ImVec2{graph_x0, body.y}, ImVec2{right, body.y + ruler_h}, true);
    {
        core::i64 step = std::max<core::i64>(
            1, static_cast<core::i64>(64.0F / std::max(px_per_frame, 0.001F)));
        constexpr std::array<core::i64, 12U> nice_steps{
            {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3600}};
        for (const core::i64 nice : nice_steps) {
            if (nice >= step) {
                step = nice;
                break;
            }
        }
        const core::i64 begin =
            (static_cast<core::i64>(impl.tl_scroll_frames) / step - 1) * step;
        for (core::i64 f = std::max<core::i64>(begin, 0);; f += step) {
            const float x = frame_to_x(static_cast<double>(f));
            if (x > right) {
                break;
            }
            if (x >= graph_x0) {
                draw->AddLine(
                    ImVec2{x, body.y + ruler_h - 6.0F},
                    ImVec2{x, body.y + ruler_h},
                    IM_COL32(90, 90, 102, 255));
                char label[16]{};
                std::snprintf(label, sizeof(label), "%lld", static_cast<long long>(f));
                draw->AddText(
                    ImVec2{x + 3.0F, body.y + 3.0F},
                    IM_COL32(150, 150, 162, 255),
                    label);
            }
        }
    }
    draw->PopClipRect();

    // Scrub over the ruler.
    ImGui::SetCursorScreenPos(ImVec2{graph_x0, body.y});
    ImGui::InvisibleButton(
        "tl_ruler", ImVec2{std::max(right - graph_x0, 1.0F), ruler_h});
    if (ImGui::IsItemActive() && px_per_frame > 0.0F) {
        const double frame = impl.tl_scroll_frames
            + (ImGui::GetMousePos().x - graph_x0) / px_per_frame;
        result.seek_to_frame = static_cast<core::u64>(
            std::clamp(
                frame, static_cast<double>(first), static_cast<double>(last))
            + 0.5);
    }

    // The flat visible row list, honouring the twirl state.
    struct Row final {
        enum class Kind { Layer, Group, Component } kind;
        doc::NodeId node;
        int group;
        int component;
        std::string label;
        ImU32 color;
        int indent;
    };
    std::vector<Row> rows;
    for (const auto& layer : layers) {
        rows.push_back(Row{
            Row::Kind::Layer, layer.node, 0, 0, layer.name, layer.color, 0});
        if (impl.expanded_layers.count(layer.node.value()) == 0U) {
            continue;
        }
        for (int g = 0; g < 3; ++g) {
            rows.push_back(Row{
                Row::Kind::Group, layer.node, g, 0, group_names[static_cast<std::size_t>(g)],
                IM_COL32(150, 150, 162, 255), 1});
            const core::u64 group_key = layer.node.value() * 4U + static_cast<core::u64>(g);
            if (impl.expanded_groups.count(group_key) == 0U) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                rows.push_back(Row{
                    Row::Kind::Component, layer.node, g, c,
                    component_names[static_cast<std::size_t>(g)][static_cast<std::size_t>(c)],
                    IM_COL32(130, 130, 140, 255), 2});
            }
        }
    }

    // Rows in a vertically-scrolling child; time stays custom (not child scroll).
    ImGui::SetCursorScreenPos(ImVec2{body.x, body.y + ruler_h});
    ImGui::BeginChild(
        "tl_rows",
        ImVec2{avail_w, std::max(avail_h - ruler_h, 1.0F)},
        false,
        ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 rows_org = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2{avail_w, static_cast<float>(rows.size()) * row_h});

    const float lane_top = body.y + ruler_h;
    const float lane_bottom = body.y + avail_h;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float y = rows_org.y + static_cast<float>(i) * row_h;
        const float cy = y + row_h * 0.5F;

        // Row background, alternating and highlighted when its layer is selected.
        const bool selected = impl.is_selected(row.node);
        const ImU32 bg = selected ? IM_COL32(48, 60, 84, 255)
                                  : ((i % 2U) ? IM_COL32(30, 30, 36, 255)
                                              : IM_COL32(26, 26, 32, 255));
        draw->AddRectFilled(
            ImVec2{rows_org.x, y}, ImVec2{right, y + row_h}, bg);

        // Label area: twirl triangle, colour swatch (layers), and name.
        const float text_x = rows_org.x + 6.0F
            + static_cast<float>(row.indent) * indent_w;
        const bool twirlable = row.kind != Row::Kind::Component;
        const core::u64 group_key =
            row.node.value() * 4U + static_cast<core::u64>(row.group);
        const bool open = row.kind == Row::Kind::Layer
            ? impl.expanded_layers.count(row.node.value()) != 0U
            : (row.kind == Row::Kind::Group
                   ? impl.expanded_groups.count(group_key) != 0U
                   : false);
        if (twirlable) {
            const float tx = text_x;
            const float ty = cy;
            if (open) {
                const ImVec2 tri[3]{
                    {tx, ty - 3.0F}, {tx + 7.0F, ty - 3.0F}, {tx + 3.5F, ty + 3.5F}};
                draw->AddTriangleFilled(
                    tri[0], tri[1], tri[2], IM_COL32(180, 180, 190, 255));
            } else {
                const ImVec2 tri[3]{
                    {tx, ty - 4.0F}, {tx + 4.0F, ty}, {tx, ty + 4.0F}};
                draw->AddTriangleFilled(
                    tri[0], tri[1], tri[2], IM_COL32(150, 150, 160, 255));
            }
        }
        const float name_x = text_x + 12.0F;
        if (row.kind == Row::Kind::Layer) {
            draw->AddRectFilled(
                ImVec2{name_x, cy - 5.0F},
                ImVec2{name_x + 10.0F, cy + 5.0F},
                row.color);
            draw->AddText(
                ImVec2{name_x + 16.0F, y + 4.0F},
                IM_COL32(225, 225, 232, 255),
                row.label.c_str());
        } else {
            draw->AddText(
                ImVec2{name_x, y + 4.0F}, row.color, row.label.c_str());
        }

        // Interaction over the label area: twirl vs select.
        ImGui::SetCursorScreenPos(ImVec2{rows_org.x, y});
        char id[32]{};
        std::snprintf(id, sizeof(id), "tl_row_%zu", i);
        if (ImGui::InvisibleButton(id, ImVec2{list_w, row_h})) {
            const bool on_twirl =
                twirlable && ImGui::GetMousePos().x < name_x;
            if (on_twirl) {
                if (row.kind == Row::Kind::Layer) {
                    if (open) {
                        impl.expanded_layers.erase(row.node.value());
                    } else {
                        impl.expanded_layers.insert(row.node.value());
                    }
                } else {
                    if (open) {
                        impl.expanded_groups.erase(group_key);
                    } else {
                        impl.expanded_groups.insert(group_key);
                    }
                }
            } else if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl) {
                impl.toggle(row.node);
            } else {
                impl.select_only(row.node);
            }
        }

        // Lane: layer bar and keyframe diamonds, clipped to the graph.
        draw->PushClipRect(
            ImVec2{graph_x0, lane_top}, ImVec2{right, lane_bottom}, true);
        if (row.kind == Row::Kind::Layer) {
            draw->AddRectFilled(
                ImVec2{frame_to_x(static_cast<double>(first)), y + 4.0F},
                ImVec2{frame_to_x(static_cast<double>(last)), y + row_h - 4.0F},
                (row.color & 0x00FFFFFFU) | 0x40000000U,
                3.0F);
        }
        const auto draw_frames =
            [&](const std::vector<core::i64>* frames, ImU32 color) {
                if (frames == nullptr) {
                    return;
                }
                for (const auto f : *frames) {
                    draw_diamond(
                        draw, frame_to_x(static_cast<double>(f)), cy, 5.0F, color);
                }
            };
        if (row.kind == Row::Kind::Component) {
            const auto channel = static_cast<scene::TransformChannel>(
                row.group * 3 + row.component);
            const auto* const comp_frames = frames_for(row.node, channel);
            draw_frames(comp_frames, IM_COL32(120, 200, 255, 255));

            // Grab a diamond to retime it: start a drag if the mouse-down lands
            // near one. The lane button owns the cursor until release.
            ImGui::SetCursorScreenPos(ImVec2{graph_x0, y});
            char lane_id[40]{};
            std::snprintf(lane_id, sizeof(lane_id), "tl_lane_%zu", i);
            ImGui::InvisibleButton(
                lane_id, ImVec2{std::max(right - graph_x0, 1.0F), row_h});
            if (ImGui::IsItemActivated() && !impl.dragging_key
                && comp_frames != nullptr) {
                const float mx = ImGui::GetMousePos().x;
                for (const auto f : *comp_frames) {
                    const float dx = frame_to_x(static_cast<double>(f)) - mx;
                    if (dx * dx <= 36.0F) {
                        impl.dragging_key = true;
                        impl.drag_key_target = row.node;
                        impl.drag_key_component = channel;
                        impl.drag_key_from = f;
                        impl.drag_key_to = f;
                        break;
                    }
                }
            }
            // The key being dragged shows as a highlighted ghost at its new time.
            if (impl.dragging_key && impl.drag_key_target == row.node
                && impl.drag_key_component == channel) {
                draw_diamond(
                    draw,
                    frame_to_x(static_cast<double>(impl.drag_key_to)),
                    cy,
                    5.0F,
                    IM_COL32(255, 220, 120, 255));
            }
        } else if (row.kind == Row::Kind::Group) {
            for (int c = 0; c < 3; ++c) {
                const auto channel = static_cast<scene::TransformChannel>(
                    row.group * 3 + c);
                draw_frames(
                    frames_for(row.node, channel), IM_COL32(90, 150, 200, 255));
            }
        }
        draw->PopClipRect();
    }
    ImGui::EndChild();

    // Author the retime on release, once, from the original frame to where the
    // diamond was dropped. A no-op move just ends the drag.
    if (impl.dragging_key && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (impl.drag_key_to != impl.drag_key_from) {
            result.key_moves.push_back(KeyMove{
                impl.drag_key_target,
                impl.drag_key_component,
                impl.drag_key_from,
                impl.drag_key_to,
                impl.next_edit_gesture++});
        }
        impl.dragging_key = false;
    }

    // Playhead across the graph, over the ruler and every lane.
    {
        const float px = frame_to_x(static_cast<double>(timeline.show_frame));
        if (px >= graph_x0 && px <= right) {
            draw->AddLine(
                ImVec2{px, body.y},
                ImVec2{px, body.y + avail_h},
                IM_COL32(240, 180, 60, 255),
                1.5F);
            draw->AddTriangleFilled(
                ImVec2{px - 5.0F, body.y},
                ImVec2{px + 5.0F, body.y},
                ImVec2{px, body.y + 7.0F},
                IM_COL32(240, 180, 60, 255));
        }
    }

    // Zoom and pan the time axis when the panel is hovered: the wheel zooms
    // about the cursor, Shift+wheel pans. The rows scroll by their scrollbar.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0F && ImGui::GetIO().KeyShift) {
            impl.tl_scroll_frames -= wheel * 8.0F;
        } else if (wheel != 0.0F && px_per_frame > 0.0F) {
            const float mouse_x = ImGui::GetMousePos().x;
            const double at = impl.tl_scroll_frames
                + (mouse_x - graph_x0) / px_per_frame;
            impl.tl_px_per_frame =
                std::clamp(px_per_frame * (1.0F + wheel * 0.15F), 0.5F, 200.0F);
            impl.tl_scroll_frames = static_cast<float>(
                at - (mouse_x - graph_x0) / impl.tl_px_per_frame);
        }
        if (impl.tl_scroll_frames < 0.0F) {
            impl.tl_scroll_frames = 0.0F;
        }
    }

    ImGui::End();
}

EditorFrame EditorShell::frame(
    const scene::Scene& scene,
    const EditorTimeline& timeline,
    const std::vector<scene::ChannelKeyframes>& keyframes,
    const std::vector<ElementInfo>& elements) noexcept {
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
    const ImGuiID dock_id =
        ImGui::DockSpaceOverViewport(0U, ImGui::GetMainViewport());

    // The default After Effects layout: viewers centre, Inspector right,
    // Timeline across the bottom. Built once; the operator's saved layout wins
    // afterwards. Only build when the dockspace is empty, so a restored
    // imgui.ini is not clobbered.
    if (impl.first_layout) {
        impl.first_layout = false;
        const auto* const node = ImGui::DockBuilderGetNode(dock_id);
        if (node == nullptr || node->IsLeafNode()) {
            ImGui::DockBuilderRemoveNode(dock_id);
            ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(
                dock_id, ImGui::GetMainViewport()->WorkSize);
            ImGuiID centre = dock_id;
            ImGuiID bottom = 0;
            ImGuiID inspector = 0;
            ImGuiID view3d = 0;
            ImGui::DockBuilderSplitNode(
                centre, ImGuiDir_Down, 0.34F, &bottom, &centre);
            ImGui::DockBuilderSplitNode(
                centre, ImGuiDir_Right, 0.22F, &inspector, &centre);
            ImGui::DockBuilderSplitNode(
                centre, ImGuiDir_Right, 0.5F, &view3d, &centre);
            ImGui::DockBuilderDockWindow("Timeline", bottom);
            ImGui::DockBuilderDockWindow("Inspector", inspector);
            ImGui::DockBuilderDockWindow("3D View", view3d);
            ImGui::DockBuilderDockWindow("Wall", centre);
            ImGui::DockBuilderFinish(dock_id);
        }
    }

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

    // Snapshot the selection's positions in `view_scene` at gesture start, so a
    // group moves rigidly - every clip by the same world delta from where it was.
    const auto capture_group = [&](const scene::Scene& view_scene) {
        impl.group_nodes.clear();
        impl.group_start.clear();
        for (const auto node : impl.selection) {
            const auto* const clip = scene::media_for(view_scene, node);
            if (clip != nullptr) {
                impl.group_nodes.push_back(node);
                impl.group_start.push_back(scene::translation_of(clip->world));
            }
        }
    };
    // One edit per captured clip, each offset from its own start by the same
    // delta, all sharing the gesture so the whole group is one undo step.
    const auto emit_group = [&](scene::Vec3 delta, core::u64 gesture) {
        for (std::size_t i = 0U; i < impl.group_nodes.size(); ++i) {
            result.edits.push_back(EditIntent{
                impl.group_nodes[i], impl.group_start[i] + delta, gesture});
        }
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
                    const bool additive =
                        ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
                    if (additive) {
                        impl.toggle(hit->source_node);
                    } else if (!impl.is_selected(hit->source_node)) {
                        // Clicking an already-selected clip keeps the group, so a
                        // drag moves all of it; clicking a new one selects it alone.
                        impl.select_only(hit->source_node);
                    } else {
                        impl.primary = hit->source_node;
                    }
                    // Only a clip that is still selected anchors a drag: a
                    // Ctrl-click that deselected the hit starts nothing.
                    if (impl.is_selected(hit->source_node)) {
                        impl.dragging_clip = true;
                        impl.begin_gesture();
                        impl.drag_start_canvas_x = vp.canvas_x;
                        impl.drag_start_canvas_y = vp.canvas_y;
                        impl.primary_start = scene::translation_of(hit->world);
                        impl.drag_camera = view_scene.camera;
                        impl.drag_canvas = canvas;
                        capture_group(view_scene);
                    }
                }
            }
            if (impl.dragging_clip && vp.active) {
                const auto delta = scene::screen_drag_to_translation(
                    impl.drag_camera,
                    impl.drag_canvas,
                    impl.primary_start,
                    vp.canvas_x - impl.drag_start_canvas_x,
                    vp.canvas_y - impl.drag_start_canvas_y);
                emit_group(delta, impl.edit_gesture);
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
            if (!impl.has_selection()) {
                return false;
            }
            // The gizmo sits on the anchor clip; a group moves rigidly with it.
            const auto* const clip = scene::media_for(scene_3d, impl.anchor());
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
                // On the first frame of the drag, snapshot the group and the
                // anchor's start; ImGuizmo reports the anchor's absolute position,
                // and the group moves by that position minus the anchor start.
                if (!impl.manipulating) {
                    capture_group(scene_3d);
                    impl.primary_start = scene::translation_of(clip->world);
                }
                const auto gesture = impl.begin_gesture();
                // Column-major: the translation column is elements 12..14.
                const scene::Vec3 moved{model_m[12], model_m[13], model_m[14]};
                emit_group(moved - impl.primary_start, gesture);
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

    // Inspector: the selection's authored transform, editable. Each changed
    // component authors a base-property edit; the whole drag is one gesture, so
    // it undoes in a step, and with several selected the anchor's values show.
    ImGui::Begin("Inspector");
    if (impl.has_selection()) {
        const auto node = impl.anchor();
        const ElementInfo* info = nullptr;
        for (const auto& element : elements) {
            if (element.node == node) {
                info = &element;
                break;
            }
        }
        if (info != nullptr) {
            ImGui::Text(
                "%s %llu",
                info->media ? "clip" : "object",
                static_cast<unsigned long long>(node.value()));
            if (impl.selection.size() > 1U) {
                ImGui::SameLine();
                ImGui::TextDisabled("(+%zu)", impl.selection.size() - 1U);
            }
            ImGui::Separator();

            const auto push_edit =
                [&](scene::TransformChannel component, double value) {
                    result.property_edits.push_back(
                        PropertyEdit{node, component, value, impl.begin_gesture()});
                };

            std::array<float, 3U> position{
                info->position.x, info->position.y, info->position.z};
            if (ImGui::DragFloat3("Position", position.data(), 0.01F)) {
                if (position[0] != info->position.x) {
                    push_edit(scene::TransformChannel::PositionX, position[0]);
                }
                if (position[1] != info->position.y) {
                    push_edit(scene::TransformChannel::PositionY, position[1]);
                }
                if (position[2] != info->position.z) {
                    push_edit(scene::TransformChannel::PositionZ, position[2]);
                }
            }
            std::array<float, 3U> rotation{
                info->rotation.x, info->rotation.y, info->rotation.z};
            if (ImGui::DragFloat3("Rotation", rotation.data(), 0.002F)) {
                if (rotation[0] != info->rotation.x) {
                    push_edit(scene::TransformChannel::RotationYaw, rotation[0]);
                }
                if (rotation[1] != info->rotation.y) {
                    push_edit(scene::TransformChannel::RotationPitch, rotation[1]);
                }
                if (rotation[2] != info->rotation.z) {
                    push_edit(scene::TransformChannel::RotationRoll, rotation[2]);
                }
            }
            std::array<float, 3U> scaling{
                info->scale.x, info->scale.y, info->scale.z};
            if (ImGui::DragFloat3("Scale", scaling.data(), 0.01F)) {
                if (scaling[0] != info->scale.x) {
                    push_edit(scene::TransformChannel::ScaleX, scaling[0]);
                }
                if (scaling[1] != info->scale.y) {
                    push_edit(scene::TransformChannel::ScaleY, scaling[1]);
                }
                if (scaling[2] != info->scale.z) {
                    push_edit(scene::TransformChannel::ScaleZ, scaling[2]);
                }
            }
            ImGui::TextDisabled("rotation in turns");
        } else {
            ImGui::TextDisabled("no transform for the selection");
        }
    } else {
        ImGui::TextDisabled("select an element");
    }
    ImGui::End();

    draw_timeline(impl, result, scene, timeline, keyframes, elements);

    // Ctrl+Z undoes the last scene edit. Because the Director groups a whole
    // drag into one transaction, this reverses the entire move in one step, not
    // frame by frame. Ignored mid-drag; a fresh id from the gesture counter tags
    // the inverse commands so it cannot collide with an edit's transaction.
    if (!impl.manipulating && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        result.undo_transaction = impl.next_edit_gesture++;
    }

    // A drag or orbit ends when the button is released, whichever view owns it.
    // Ending the manipulation closes the gesture, so the next drag is a new undo
    // step; the gizmo latches its own drag, so IsUsing falls with the button too.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        impl.dragging_clip = false;
        impl.orbiting = false;
        impl.manipulating = false;
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
