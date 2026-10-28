#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>

#include <SDL3/SDL.h>

#include <chrono>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#endif

namespace {
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error{reason};
}

gui::UiId inspector_id(std::string_view name, std::size_t salt) {
  gui::UiId id = gui::ui_id("editor.inspector") ^ 0x9E3779B97F4A7C15ULL;
  for (char c : name) { id ^= static_cast<unsigned char>(c); id *= 1099511628211ULL; }
  id ^= salt + 0x9E3779B9ULL;
  return id * 1099511628211ULL;
}

void workflow(const char* project_path, const char* workspace_path) {
  if (!editor::editor_available()) {
    std::puts("[skip] editor GPU backend unavailable");
    return;
  }
#if defined(_WIN32)
  if (GetEnvironmentVariableA("MS_EDITOR_GPU_VALIDATE", nullptr, 0) != 0) {
    ID3D12Debug* debug = nullptr;
    check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))), "D3D12 debug interface");
    debug->EnableDebugLayer();
    debug->Release();
  }
#endif
  editor::EditorConfig config;
  config.title = "Editor workflow regression";
  std::vector<std::byte> saved_workspace;
  if (workspace_path) {
    auto loaded = mediaserver::persist::load_workspace_layout(workspace_path);
    check(loaded.has_value(), "load reproduction workspace");
    saved_workspace = std::move(loaded).value();
    config.workspace_layout = saved_workspace;
  }
  auto created = editor::EditorShell::create(config);
  check(created.has_value(), "create editor");
  auto shell = std::move(created).value();
  scene::Scene show;
  show.id = scene::SceneId{100U};
  editor::EditorTimeline timeline;
  timeline.last_frame = 600U;
  std::vector<scene::ChannelKeyframes> keyframes;
  editor::ElementInfo object;
  object.node = doc::NodeId{5U};
  object.kind = editor::ElementKind::Object;
  object.display_name = "Selected object";
  scene::MeshInstance mesh;
  mesh.source = object.node;
  mesh.mesh = {scene::Primitive::Box, 1U};
  mesh.world = mesh.normal = scene::identity();
  show.instances.push_back(mesh);
  show.camera.eye = {0, 0, 4};
  show.camera.target = {0, 0, 0};
  std::vector<editor::ElementInfo> elements{object};
  if (project_path != nullptr) {
    const auto loaded = mediaserver::persist::load_project_file(project_path);
    check(loaded.has_value(), "load reproduction project");
    scene::SceneModel model;
    model.reset(loaded.value());
    show = model.evaluate({});
    keyframes = model.channel_keyframes();
    elements.clear();
    auto named = [&](editor::ElementInfo& info) {
      if (const auto* node = loaded.value().get(info.node)) {
        const auto value = node->properties.find("name");
        if (value != node->properties.end())
          if (const auto* name = std::get_if<std::string>(&value->second)) info.display_name = *name;
      }
    };
    for (const auto& instance : show.instances) {
      editor::ElementInfo info; info.kind=editor::ElementKind::Object; info.node=instance.source;
      info.material=instance.material; info.style=instance.style; info.locked=instance.locked;
      named(info); elements.push_back(std::move(info));
    }
    for (const auto& instance : show.models) {
      editor::ElementInfo info; info.kind=editor::ElementKind::Model; info.node=instance.source_node;
      info.source=instance.source; info.material=instance.material; info.style=instance.style; info.locked=instance.locked;
      named(info); elements.push_back(std::move(info));
    }
    check(!elements.empty(), "project has a selectable drawable");
    object = elements.front();
    std::printf("project: %zu meshes, %zu models, selected %llu\n", show.instances.size(), show.models.size(),
                static_cast<unsigned long long>(object.node.value()));
    for (const auto& instance : show.models)
      std::printf("model: %s exists=%d\n", instance.source.c_str(), std::filesystem::exists(instance.source));
  }
  gui::DockLayout layout;
  std::vector<editor::EffectIntent> requested_effects;
  const std::array<std::string, 1U> recent_projects{"C:/Shows/Recent.mstage"};
  editor::EditorProjectState project_state{};
  project_state.recent_projects = recent_projects;
  std::optional<std::string> requested_project;
  mediaserver::core::FrameTiming completed_timing;
  float scale = 1.0F;
  SDL_Window* window = nullptr;
  const auto adopt_layout = [&](const std::vector<std::byte>& bytes) {
    const auto decoded = editor::decode_workspace_layout(bytes);
    check(decoded.has_value(), "decode live layout");
    for (const auto& node : decoded.value().docks.nodes)
      for (auto id : node.tabs) layout.register_panel(id, "Panel");
    for (const auto& closed : decoded.value().docks.closed) layout.register_panel(closed.panel, "Panel");
    check(layout.restore(decoded.value().docks), "restore live layout");
    scale = SDL_GetWindowDisplayScale(window) * static_cast<float>(decoded.value().ui_zoom_percent) / 100.0F;
    int width = 0, height = 0;
    check(SDL_GetWindowSizeInPixels(window, &width, &height), "window extent");
    layout.solve(editor::editor_dock_area(static_cast<float>(width) / scale,
                                          static_cast<float>(height) / scale),
                 {.divider=6, .seam_reach=4, .tab_bar=32, .title_bar=30, .edge=0.25F});
  };
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i)
    if (std::string{SDL_GetWindowTitle(windows[i])} == config.title) window = windows[i];
  SDL_free(windows);
  check(window != nullptr, "find editor window");
  scale = SDL_GetWindowDisplayScale(window);
  if (!saved_workspace.empty()) adopt_layout(saved_workspace);
  editor::EditorSwitcherState switcher;
  auto frame = [&](const char* step) {
    std::printf("frame: %s\n", step); std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    auto result = shell->frame(show, timeline, keyframes, elements,
                               {}, {}, {}, switcher, {}, {}, {}, project_state);
    if (result.load_project) requested_project = result.load_project->path;
    completed_timing = result.frame_timing;
    requested_effects.insert(requested_effects.end(), result.effects.begin(), result.effects.end());
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    char diagnostic[512]{};
    std::snprintf(diagnostic, sizeof(diagnostic), "completed: %s %.2f ms; assets=%d/%llu GPU=%llu valid=%d %.2f ms pass=%s\n", step, elapsed,
        result.assets.ready, static_cast<unsigned long long>(result.assets.digest),
        static_cast<unsigned long long>(result.frame_timing.sequence), result.frame_timing.gpu_valid,
        static_cast<double>(result.frame_timing.gpu_ns)/1e6,
        mediaserver::core::render_pass_names[static_cast<std::size_t>(result.frame_timing.slowest_pass())].data());
    std::fputs(diagnostic, stdout);
#if defined(_WIN32)
    if (GetEnvironmentVariableA("MS_EDITOR_GPU_VALIDATE", nullptr, 0) != 0) OutputDebugStringA(diagnostic);
#endif
    std::fflush(stdout);
    check(result.status == editor::EditorStatus::Running, "editor still running");
    if (result.save_workspace_layout) {
      adopt_layout(*result.save_workspace_layout);
    }
    return result;
  };
  frame("initial");
  const SDL_WindowID window_id = SDL_GetWindowID(window);
  const doc::NodeId selected[]{object.node};
  check(shell->apply_selection_set_recall(show.id, selected), "select object");
  frame("selected object");
  auto click = [&](float x, float y, const char* step) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = window_id; event.button.button = SDL_BUTTON_LEFT;
    const float input_scale = scale / SDL_GetWindowPixelDensity(window);
    event.button.down = true; event.button.x = x * input_scale; event.button.y = y * input_scale;
    check(SDL_PushEvent(&event), "push press"); frame(step);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP; event.button.down = false;
    check(SDL_PushEvent(&event), "push release"); frame(step);
    frame("settled");
  };
  // The File menu is the first entry of the menu bar; its popup opens at
  // the top of the dock area under it, Recent Projects its third row, and
  // the submenu beside it.
  const float popup_x = editor::editor_menu_bar_left;
  const float popup_y = editor::editor_top_bar_height;
  click(popup_x + 12.0F, editor::editor_top_bar_height * 0.5F, "File menu");
  SDL_Event hover_recent{};
  hover_recent.type = SDL_EVENT_MOUSE_MOTION;
  hover_recent.motion.windowID = window_id;
  const float input_scale = scale / SDL_GetWindowPixelDensity(window);
  hover_recent.motion.x = (popup_x + 22.0F) * input_scale;
  hover_recent.motion.y = (popup_y + 59.0F) * input_scale;
  check(SDL_PushEvent(&hover_recent), "hover Recent Projects");
  frame("Recent Projects submenu hover");
  check(!requested_project.has_value(),
        "opening Recent Projects does not load a project");
  click(popup_x + 242.0F, popup_y + 63.0F, "Recent project");
  check(requested_project == recent_projects.front(),
        "recent project uses the editor's project-open request");
  const auto properties = gui::ui_id("editor.panel.inspector");
  const auto inspector_bounds = layout.content_of(layout.leaf_of(properties));
  const auto font = [&] {
    constexpr std::array candidates{
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/local/share/fonts/DejaVuSans.ttf"};
    for (const auto* path : candidates)
      if (auto atlas = gui::FontAtlas::create(path, 14U, scale)) return std::move(atlas).value();
    return gui::FontAtlas::empty();
  }();
  gui::InputState input;
  gui::UiDrawList list;
  gui::Theme theme; theme.padding = 5.0F;
  gui::UiContext ctx{input, list, font, theme};
  editor::InspectorPanel inspector{gui::ui_id("editor.inspector")};
  bool found = false;
  for (float y = inspector_bounds.y; y < inspector_bounds.y + inspector_bounds.height; y += 2) {
    input.new_frame(); input.set_mouse_position(inspector_bounds.x + 40, y); ctx.new_frame();
    static_cast<void>(inspector.draw(ctx, inspector_bounds, &object));
    if (ctx.hot() == inspector_id("effects.add", object.node.value())) {
      click(inspector_bounds.x + 40, y, "Add effect"); found = true; break;
    }
  }
  check(found, "find Add effect");
  check(requested_effects.empty(), "opening picker does not author an effect");
  const auto key = [&](SDL_Keycode code) {
    SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window_id; event.key.key = code;
    event.key.down = true;
    check(SDL_PushEvent(&event), "push picker key"); frame("picker key");
    event.type = SDL_EVENT_KEY_UP; event.key.down = false;
    check(SDL_PushEvent(&event), "release picker key"); frame("picker key release");
  };
  key(SDLK_DOWN); key(SDLK_DOWN); key(SDLK_RETURN);
  check(requested_effects.size() == 1, "picker authors one effect");
  check(requested_effects.front().target == object.node &&
        requested_effects.front().kind == scene::EffectKind::Saturation &&
        requested_effects.front().gesture != 0, "choice targets selection with undo gesture");
  frame("effect chosen with selected object");
  const auto properties_sequence = completed_timing.sequence;
  for (int i = 0; i < 100; ++i) { SDL_Delay(8); frame("Properties hover"); }
  check(completed_timing.gpu_valid && completed_timing.sequence > properties_sequence,
        "GPU completes frames after effect picker");
  const auto cues = gui::ui_id("editor.panel.cues");
  const auto cue_leaf = layout.leaf_of(cues);
  const auto cue_rect = layout.rect_of(cue_leaf);
  // Timeline precedes Cues in the starting workspace.
  click(cue_rect.x + font.measure("Timeline") + 5.0F * 3.0F + 20.0F + 15.0F,
        cue_rect.y + 14.0F, "Cues tab");
  check(layout.active_of(cue_leaf) == cues, "Cues tab activated");
  frame("Cues with selected object");
  const auto cues_sequence = completed_timing.sequence;
  for (int i = 0; i < 100; ++i) { SDL_Delay(8); frame("Cues hover"); }
  check(completed_timing.gpu_valid && completed_timing.sequence > cues_sequence,
        "GPU completes frames after Cues tab switch");
  check(shell->apply_selection_set_recall(show.id, selected),
        "select object before linked-source removal");
  elements.clear();
  show.instances.clear();
  frame("selected source object removed");
  SDL_Event removed_key{};
  removed_key.type = SDL_EVENT_KEY_DOWN;
  removed_key.key.windowID = window_id;
  removed_key.key.key = SDLK_DELETE;
  removed_key.key.down = true;
  check(SDL_PushEvent(&removed_key), "press Delete after source removal");
  check(frame("Delete after source removal").deletes.empty(),
        "a removed source cannot be deleted again through stale selection");
  removed_key.type = SDL_EVENT_KEY_UP;
  removed_key.key.down = false;
  check(SDL_PushEvent(&removed_key), "release Delete after source removal");
  frame("removed source settled");

  // A cold model on Stage must report its own preparation even though
  // Program has no reference to it. Drive the shipping frame/UI path through
  // decode, upload, readiness, and a failed source without a second renderer.
  const auto fixture_dir = std::filesystem::current_path() / "out" /
      ("stage-loading-" + std::to_string(SDL_GetTicksNS()));
  std::filesystem::create_directories(fixture_dir);
  const auto model_path = fixture_dir / "triangle.obj";
  {
    std::ofstream file{model_path};
    file << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";
    check(static_cast<bool>(file), "write Stage loading fixture");
  }
  scene::Scene stage;
  stage.id = scene::SceneId{901U};
  scene::ModelInstance stage_model;
  stage_model.source_node = doc::NodeId{902U};
  stage_model.source = model_path.string();
  stage_model.world = stage_model.normal = scene::identity();
  stage.models.push_back(stage_model);
  switcher.stage_scene = &stage;
  bool saw_loading = false;
  bool saw_ready = false;
  const auto ready_deadline = SDL_GetTicks() + 5000U;
  while (SDL_GetTicks() < ready_deadline) {
    const auto observed = frame("Stage model preparation");
    saw_loading = saw_loading || observed.stage_assets.loading;
    if (saw_loading && observed.stage_assets.headline.empty()) {
      saw_ready = true;
      break;
    }
    SDL_Delay(5);
  }
  check(saw_loading, "cold Stage model displays loading notice");
  check(saw_ready, "Stage loading notice clears after model is ready");
  stage.models.front().source = (fixture_dir / "missing.obj").string();
  bool saw_failure = false;
  const auto failure_deadline = SDL_GetTicks() + 5000U;
  while (SDL_GetTicks() < failure_deadline) {
    const auto observed = frame("Stage missing model");
    if (!observed.stage_assets.loading &&
        observed.stage_assets.headline == "STAGE ASSET LOAD FAILED") {
      saw_failure = true;
      break;
    }
    SDL_Delay(5);
  }
  check(saw_failure, "missing Stage model shows failure instead of endless loading");
  stage.models.clear();
  bool cleared = false;
  const auto clear_deadline = SDL_GetTicks() + 5000U;
  while (SDL_GetTicks() < clear_deadline) {
    if (frame("Stage failed source removed").stage_assets.headline.empty()) {
      cleared = true;
      break;
    }
    SDL_Delay(5);
  }
  check(cleared, "removing failed model clears Stage failure notice");
}
} // namespace

int main(int argc, char** argv) {
  try { workflow(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr); std::puts("[pass] selected-object panel navigation"); return 0; }
  catch (const std::exception& e) { std::fprintf(stderr, "[fail] %s\n", e.what()); return 1; }
}
