#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace show = mediaserver::show;
namespace ui = mediaserver::ui;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define PRESENTER_CHECK(...)                                                   \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

void advanced_scene_reaches_the_native_application_presenter() {
  ui::SdlPresenterConfig config{};
  config.title = "media-server Advanced presenter test";
  config.width = 128U;
  config.height = 96U;
  config.resizable = false;
  config.gpu = true;
  config.scene = true;

  auto created = ui::SdlPresenter::create(config);
  PRESENTER_CHECK(created);
  std::unique_ptr<show::Presenter> owned = std::move(created).value();
  auto &presenter = static_cast<ui::SdlPresenter &>(*owned);
  PRESENTER_CHECK(presenter.backend() == rhi::BackendKind::D3d12);

  scene::Scene frame{};
  frame.canvas = scene::Canvas{config.width, config.height};
  frame.environment.sky_color = scene::Vec3{0.03F, 0.24F, 0.78F};
  frame.environment.ground_color = scene::Vec3{0.03F, 0.24F, 0.78F};
  frame.renderer_settings.backend = scene::RendererBackend::Advanced;
  frame.renderer_settings.required = true;

  PRESENTER_CHECK(presenter.present(frame, scene::whole(frame.canvas)) ==
                  show::PresenterStatus::Presented);
  const render::RendererReadiness readiness = presenter.render_readiness();
  PRESENTER_CHECK(readiness.satisfied);
  PRESENTER_CHECK(!readiness.fallback);
  PRESENTER_CHECK(readiness.selected == scene::RendererBackend::Advanced);
  const show::PresenterRendererReadiness reported =
      presenter.renderer_readiness();
  PRESENTER_CHECK(reported.profile_digest ==
                  scene::renderer_settings_digest(frame.renderer_settings));
  PRESENTER_CHECK(reported.state ==
                  show::PresenterRendererReadinessState::Ready);
  PRESENTER_CHECK(reported.selected == scene::RendererBackend::Advanced);

  // readback() is an explicit diagnostic operation and may wait. It digests
  // the connector-ready target after the Advanced linear scene has passed
  // through the output transfer, proving that path produced real pixels.
  const show::PresenterReadback pixels = presenter.readback();
  PRESENTER_CHECK(pixels);
  PRESENTER_CHECK(pixels.width == config.width);
  PRESENTER_CHECK(pixels.height == config.height);
  PRESENTER_CHECK(pixels.digest != 0U);
}

void two_native_outputs_publish_bakes_together_and_a_late_output_holds() {
  ui::SdlPresenterConfig config{};
  config.title = "media-server baked cohort test";
  config.width = 64;
  config.height = 48;
  config.gpu = config.scene = true;
  config.resizable = false;
  std::array<std::unique_ptr<show::Presenter>, 3> owned;
  std::array<ui::SdlPresenter *, 3> outputs{};
  for (std::size_t i = 0; i < owned.size(); ++i) {
    auto created = ui::SdlPresenter::create(config);
    PRESENTER_CHECK(created);
    owned[i] = std::move(created).value();
    outputs[i] = static_cast<ui::SdlPresenter *>(owned[i].get());
  }
  scene::Scene frame;
  frame.canvas = {config.width, config.height};
  frame.camera.eye = {0, 0, 4};
  frame.camera.target = {0, 0, 0};
  frame.renderer_settings.backend = scene::RendererBackend::Advanced;
  frame.renderer_settings.required = true;
  frame.asset_cohort_required = true;
  frame.control_frame = 10;
  frame.show_frame = 7;
  auto &grid = frame.environment.baked;
  grid.minimum = {-2, -2, -2};
  grid.maximum = {2, 2, 2};
  grid.count_x = grid.count_y = grid.count_z = 2;
  grid.samples = 256;
  const auto make_field = [&](scene::Vec3 color, const char *name) {
    auto input = frame;
    input.environment.sky_color = input.environment.ground_color = color;
    const auto baked = render::bake_lighting(input);
    PRESENTER_CHECK(baked.ready());
    const auto saved = render::write_probe_asset(
        *baked.asset, (std::filesystem::temp_directory_path() / name).string());
    PRESENTER_CHECK(saved.ready());
    return saved;
  };
  const auto first = make_field({0.8F, 0.02F, 0.01F}, "native-cohort-red");
  const auto second = make_field({0.01F, 0.03F, 0.8F}, "native-cohort-blue");
  frame.environment.sky_color = frame.environment.ground_color = {};
  scene::MeshInstance sphere;
  sphere.mesh = {scene::Primitive::Sphere, 16};
  sphere.world = sphere.normal = scene::identity();
  sphere.material.base_color = {1, 1, 1};
  sphere.material.roughness = 1;
  frame.instances.push_back(sphere);
  grid.enabled = true;
  grid.source = first.source;
  grid.content = first.asset->content;

  const auto present = [&](ui::SdlPresenter &output) {
    auto status = show::PresenterStatus::Skipped;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    do {
      const auto submissions = output.timing().frames;
      status = output.present(frame, scene::whole(frame.canvas));
      // Busy means no candidate was recorded. An asset-deferred submission,
      // however, must remain observable to the publication assertions below.
      if (status != show::PresenterStatus::Deferred ||
          output.timing().frames != submissions)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    return status;
  };
  const auto prepare = [&](ui::SdlPresenter &output) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    do {
      static_cast<void>(present(output));
      if (output.renderer_readiness().assets.ready)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    PRESENTER_CHECK(false && "asset uploads completed");
  };
  for (auto *output : outputs) {
    prepare(*output);
    PRESENTER_CHECK(!output->readback());
  }
  frame.asset_commit = {scene::asset_set_digest(frame), 100, 77};
  frame.control_frame = 99;
  frame.show_frame = 76;
  for (auto *output : outputs)
    PRESENTER_CHECK(present(*output) == show::PresenterStatus::Deferred);
  frame.control_frame = 100;
  frame.show_frame = 77;
  std::array<mediaserver::core::u64, 3> old_pixels{};
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    PRESENTER_CHECK(present(*outputs[i]) == show::PresenterStatus::Presented);
    PRESENTER_CHECK(outputs[i]->renderer_readiness().assets.visible_commit ==
                    100);
    old_pixels[i] = outputs[i]->readback().digest;
    PRESENTER_CHECK(old_pixels[i] != 0 && old_pixels[i] == old_pixels[0]);
  }

  grid.source = second.source;
  grid.content = second.asset->content;
  frame.control_frame = 150;
  frame.show_frame = 127;
  for (std::size_t i = 0; i < 2; ++i) {
    prepare(*outputs[i]);
    PRESENTER_CHECK(outputs[i]->readback().digest == old_pixels[i]);
  }
  frame.asset_commit = {scene::asset_set_digest(frame), 200, 177};
  frame.control_frame = 200;
  frame.show_frame = 177;
  mediaserver::core::u64 new_pixels{};
  for (std::size_t i = 0; i < 2; ++i) {
    PRESENTER_CHECK(present(*outputs[i]) == show::PresenterStatus::Presented);
    PRESENTER_CHECK(outputs[i]->renderer_readiness().assets.visible_commit ==
                    200);
    const auto pixels = outputs[i]->readback();
    PRESENTER_CHECK(pixels && pixels.digest != old_pixels[i]);
    if (i == 0)
      new_pixels = pixels.digest;
    else
      PRESENTER_CHECK(pixels.digest == new_pixels);
  }
  // This member reaches N+1 only on activation. Its GPU upload has not
  // completed, so it must retain N even after the field becomes ready.
  PRESENTER_CHECK(present(*outputs[2]) == show::PresenterStatus::Deferred);
  PRESENTER_CHECK(outputs[2]->readback().digest == old_pixels[2]);
  frame.control_frame = 201;
  frame.show_frame = 178;
  prepare(*outputs[2]);
  PRESENTER_CHECK(present(*outputs[2]) == show::PresenterStatus::Deferred);
  PRESENTER_CHECK(outputs[2]->readback().digest == old_pixels[2]);
  PRESENTER_CHECK(outputs[2]->renderer_readiness().assets.visible_commit == 0);
}

void busy_native_output_defers_and_hold_retires_without_rendering() {
  ui::SdlPresenterConfig config{};
  config.title = "media-server buffered GPU submission test";
  config.width = 1920;
  config.height = 1080;
  config.resizable = false;
  config.gpu = config.scene = config.output = config.buffered_media = true;
  auto created = ui::SdlPresenter::create(config);
  PRESENTER_CHECK(created);
  auto owned = std::move(created).value();
  auto &presenter = static_cast<ui::SdlPresenter &>(*owned);

  scene::Scene frame;
  frame.canvas = {config.width, config.height};
  frame.renderer_settings.backend = scene::RendererBackend::Advanced;
  frame.renderer_settings.required = true;
  frame.renderer_settings.motion_blur = true;
  frame.environment.sky_color = frame.environment.ground_color = {0.1F, 0.2F, 0.3F};
  mediaserver::output::OutputMap map;
  map.node = mediaserver::doc::NodeId{1};
  map.source = map.viewport = scene::whole(frame.canvas);

  mediaserver::core::u64 busy_polls{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
  while (frame.show_frame < 8 && std::chrono::steady_clock::now() < deadline) {
    const auto submissions = presenter.timing().frames;
    const auto skipped = presenter.timing().skipped_presents;
    const auto status = presenter.present(frame, map);
    PRESENTER_CHECK(status != show::PresenterStatus::Failed &&
                    status != show::PresenterStatus::Closed);
    if (presenter.timing().frames == submissions) {
      ++busy_polls;
      PRESENTER_CHECK(status == show::PresenterStatus::Deferred);
      PRESENTER_CHECK(presenter.timing().skipped_presents == skipped);
    } else if (status == show::PresenterStatus::Presented ||
               status == show::PresenterStatus::Skipped) {
      ++frame.show_frame;
      ++frame.control_frame;
    }
  }
  PRESENTER_CHECK(frame.show_frame == 8);
  std::cout << "      GPU busy polls checked: " << busy_polls << '\n';

  const auto submissions = presenter.timing().frames;
  while (presenter.renderer_readiness().timing.sequence < submissions &&
         std::chrono::steady_clock::now() < deadline) {
    PRESENTER_CHECK(presenter.hold() == show::PresenterStatus::Skipped);
    std::this_thread::yield();
  }
  PRESENTER_CHECK(presenter.timing().frames == submissions);
  PRESENTER_CHECK(presenter.renderer_readiness().timing.sequence == submissions);
  PRESENTER_CHECK(presenter.timing().last_submit_to_observed_ns > 0);
  PRESENTER_CHECK(presenter.readback());
}

void fullscreen_output_keeps_presenting_without_keyboard_focus() {
  PRESENTER_CHECK(SDL_Init(SDL_INIT_VIDEO));
  // Start with SDL's normal policy, including when another test set a hint.
  PRESENTER_CHECK(SDL_SetHintWithPriority(
      SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "auto", SDL_HINT_OVERRIDE));
  int display_count{};
  auto *displays = SDL_GetDisplays(&display_count);
  PRESENTER_CHECK(displays != nullptr && display_count > 0);
  const auto display = displays[display_count - 1];
  SDL_free(displays);
  const auto *desktop = SDL_GetDesktopDisplayMode(display);
  PRESENTER_CHECK(desktop != nullptr);
  SDL_Rect bounds{};
  PRESENTER_CHECK(SDL_GetDisplayBounds(display, &bounds));

  ui::SdlPresenterConfig config{};
  config.title = "media-server fullscreen focus test";
  config.width = static_cast<mediaserver::core::u32>(desktop->w);
  config.height = static_cast<mediaserver::core::u32>(desktop->h);
  config.refresh_numerator = static_cast<mediaserver::core::u32>(desktop->refresh_rate_numerator);
  config.refresh_denominator = static_cast<mediaserver::core::u32>(desktop->refresh_rate_denominator);
  config.position_x = bounds.x;
  config.position_y = bounds.y;
  config.position_explicit = config.fullscreen = config.require_exact_display_mode = true;
  config.gpu = config.scene = config.output = true;
  config.resizable = false;
  auto created = ui::SdlPresenter::create(config);
  PRESENTER_CHECK(created);
  auto owned = std::move(created).value();
  auto &presenter = static_cast<ui::SdlPresenter &>(*owned);
  auto *output_window = SDL_GetWindowFromID(presenter.window_id());
  PRESENTER_CHECK(output_window != nullptr);
  PRESENTER_CHECK(SDL_GetWindowFullscreenMode(output_window) != nullptr);
  PRESENTER_CHECK(SDL_RaiseWindow(output_window));
  PRESENTER_CHECK(SDL_SyncWindow(output_window));
  SDL_PumpEvents();
  PRESENTER_CHECK(SDL_GetWindowFlags(output_window) & SDL_WINDOW_INPUT_FOCUS);

  // A real second window takes focus, as the editor does during playback.
  auto focus_window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>{
      SDL_CreateWindow("media-server operator focus test", 320, 200, 0),
      &SDL_DestroyWindow};
  PRESENTER_CHECK(focus_window);
  PRESENTER_CHECK(SDL_RaiseWindow(focus_window.get()));
  PRESENTER_CHECK(SDL_SyncWindow(focus_window.get()));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (SDL_GetKeyboardFocus() != focus_window.get() &&
         std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(presenter.hold());
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  PRESENTER_CHECK(SDL_GetKeyboardFocus() == focus_window.get());

  scene::Scene frame;
  frame.canvas = {config.width, config.height};
  frame.renderer_settings.backend = scene::RendererBackend::Advanced;
  frame.renderer_settings.required = true;
  frame.environment.sky_color = frame.environment.ground_color = {0.1F, 0.2F, 0.3F};
  mediaserver::output::OutputMap map;
  map.node = mediaserver::doc::NodeId{1};
  map.source = map.viewport = scene::whole(frame.canvas);
  while (frame.show_frame < 8 && std::chrono::steady_clock::now() < deadline) {
    const auto status = presenter.present(frame, map);
    PRESENTER_CHECK(status != show::PresenterStatus::Failed &&
                    status != show::PresenterStatus::Closed);
    PRESENTER_CHECK(!(SDL_GetWindowFlags(output_window) & SDL_WINDOW_MINIMIZED));
    PRESENTER_CHECK(SDL_GetKeyboardFocus() == focus_window.get());
    if (status == show::PresenterStatus::Presented) {
      ++frame.show_frame;
      ++frame.control_frame;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  PRESENTER_CHECK(frame.show_frame == 8);
  PRESENTER_CHECK(presenter.readback());
}

} // namespace

int main() {
  if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
    std::cout << "[skip] native D3D12 device unavailable\n";
    return 0;
  }
  try {
    advanced_scene_reaches_the_native_application_presenter();
    std::cout << "[pass] Advanced scene reached native presenter\n";
    two_native_outputs_publish_bakes_together_and_a_late_output_holds();
    std::cout << "[pass] native bake cohort pixels and late-node hold\n";
    busy_native_output_defers_and_hold_retires_without_rendering();
    std::cout << "[pass] busy output defers and held output retires GPU work\n";
    fullscreen_output_keeps_presenting_without_keyboard_focus();
    std::cout << "[pass] exclusive fullscreen output presents without keyboard focus\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "[fail] " << error.what() << '\n';
    return 1;
  }
}
