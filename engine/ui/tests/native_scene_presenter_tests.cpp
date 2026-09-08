#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

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
      status = output.present(frame, scene::whole(frame.canvas));
      if (status != show::PresenterStatus::Skipped)
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
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "[fail] " << error.what() << '\n';
    return 1;
  }
}
