#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
  using namespace mediaserver;
  if (argc < 3 || argc > 5) {
    std::cerr << "Usage: media-bake project.mstage output-base [scene-id] "
                 "[show-frame]\n"
                 "Uses the scene's authored probe bounds, density, samples and "
                 "bounce count.\n";
    return 2;
  }
  const std::string file = argv[1];
  const auto loaded = persist::load_project_file(
      std::filesystem::path{std::u8string(file.begin(), file.end())});
  if (!loaded) {
    std::cerr << "Could not load project\n";
    return 1;
  }
  scene::SceneModel model;
  model.reset(loaded.value());
  const auto scenes = model.scenes();
  if (scenes.empty()) {
    std::cerr << "Project has no Stage scene\n";
    return 1;
  }
  core::u64 scene_id = scenes.front().id.value(), frame = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string_view value = argv[i];
    auto &number = i == 3 ? scene_id : frame;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      std::cerr << "Invalid numeric argument\n";
      return 2;
    }
  }
  if (std::none_of(scenes.begin(), scenes.end(),
                   [&](const auto &s) { return s.id.value() == scene_id; })) {
    std::cerr << "Scene id does not exist in the project\n";
    return 2;
  }
  const auto transport = show::read_transport(loaded.value());
  const auto rate = transport ? transport->rate : show::FrameRate{60, 1};
  const auto snapshot =
      model.evaluate(scene::SceneId{scene_id},
                     scene::SceneTime{frame, rate.numerator, rate.denominator});
  const auto started = std::chrono::steady_clock::now();
  auto job = render::BakeJob::start(snapshot, argv[2]);
  if (!job) {
    std::cerr << "Could not start bake\n";
    return 1;
  }
  core::u32 last = ~0U;
  render::BakeStage stage = render::BakeStage::Cancelled;
  while (!job->result()) {
    const auto now = job->progress().stage.load();
    const auto completed = job->progress().completed.load();
    if (now != stage || completed != last) {
      std::cout << render::bake_stage_label(now) << " " << completed << "/"
                << job->progress().total.load() << '\n';
      stage = now;
      last = completed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
  }
  const auto &result = *job->result();
  if (!result.ready()) {
    std::cerr << result.error << '\n';
    return 1;
  }
  std::cout << "Asset: " << result.source
            << "\nContent: " << result.asset->content << "\nSeconds: "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             started)
                   .count()
            << '\n';
  return 0;
}
