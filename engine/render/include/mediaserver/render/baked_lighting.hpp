#pragma once

#include <mediaserver/scene/scene.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mediaserver::render {

// Nine radiance SH coefficients, then an 8x8 octahedral visibility map packed
// as 32 (mean, second moment, mean, second moment) vectors. Shared GPU layout.
inline constexpr core::u32 probe_sh_count = 9U;
inline constexpr core::u32 probe_visibility_side = 8U;
inline constexpr core::u32 probe_vector_count = 41U;
struct BakedProbe final {
  std::array<scene::Vec4, probe_vector_count> values{};
  bool operator==(const BakedProbe &) const = default;
};
static_assert(sizeof(BakedProbe) == probe_vector_count * 16U);

struct ProbeAsset final {
  scene::BakedLighting grid;
  std::vector<BakedProbe> probes;
  std::string content;
  // Fixed equirectangular radiance captures. Runtime uses the identical GGX
  // prefilter as the environment, with no camera-dependent selection.
  std::array<std::vector<scene::Vec4>, scene::max_reflection_probes> reflections;
};
inline constexpr core::u32 reflection_capture_width = 256U;
inline constexpr core::u32 reflection_capture_height = 128U;
inline constexpr core::u32 reflection_capture_texels = reflection_capture_width * reflection_capture_height;

enum class BakeStage : core::u8 {
  Queued,
  Reading,
  Tracing,
  Writing,
  Ready,
  Failed,
  Cancelled
};
struct BakeProgress final {
  std::atomic<BakeStage> stage{BakeStage::Queued};
  std::atomic_uint32_t completed{};
  std::atomic_uint32_t total{};
  std::atomic_bool cancel{};
};
struct BakeResult final {
  std::shared_ptr<const ProbeAsset> asset;
  std::string source;
  std::string error;
  [[nodiscard]] bool ready() const noexcept {
    return asset != nullptr && error.empty();
  }
};

// Offline only. Fixed sample order/seed, no clock or thread identity in the
// integrator or bytes. The scene is an evaluated snapshot of the chosen frame.
[[nodiscard]] BakeResult bake_lighting(const scene::Scene &scene,
                                       BakeProgress *progress = nullptr);
// Snapshot identity excludes camera motion, dynamic contributors and the old
// baked field. Used to reject a completed bake after its static inputs changed.
[[nodiscard]] core::u64 bake_input_digest(const scene::Scene &scene);
[[nodiscard]] std::vector<core::u8> encode_probe_asset(const ProbeAsset &asset);
[[nodiscard]] BakeResult decode_probe_asset(std::span<const core::u8> bytes);
[[nodiscard]] BakeResult load_probe_asset(const std::string &source,
                                          const std::string &expected_content);
[[nodiscard]] BakeResult write_probe_asset(const ProbeAsset &asset,
                                           const std::string &destination);
[[nodiscard]] scene::Vec3 sample_probe_irradiance(const ProbeAsset &asset,
                                                  scene::Vec3 position,
                                                  scene::Vec3 normal) noexcept;
[[nodiscard]] std::string_view bake_stage_label(BakeStage stage) noexcept;

// An operator request. One process worker; at most two outstanding snapshots.
// Releasing a handle never waits for tracing or disk. The owner fences results
// against the project/stage which submitted them before authoring a command.
class BakeJob final {
public:
  [[nodiscard]] static std::shared_ptr<BakeJob> start(scene::Scene scene,
                                                      std::string destination);
  [[nodiscard]] const BakeProgress &progress() const noexcept {
    return progress_;
  }
  void cancel() noexcept { progress_.cancel.store(true); }
  [[nodiscard]] const BakeResult *result() const noexcept;

private:
  struct Executor;
  scene::Scene scene_;
  std::string destination_;
  BakeProgress progress_;
  BakeResult result_;
};

enum class ProbeCacheState : core::u8 {
  Unseen,
  Pending,
  Ready,
  Failed,
  Busy,
  Capacity
};
struct ProbeObservation final {
  std::shared_ptr<const ProbeAsset> asset;
  ProbeCacheState state{ProbeCacheState::Unseen};
  core::u64 generation{};
  core::u64 publication_epoch{};
};
struct ProbeCacheStats final {
  core::u64 admissions{}, failures{}, late_results{}, busy{}, refused{};
};
class ProbeCache final {
public:
  ProbeCache();
  [[nodiscard]] ProbeObservation
  observe(const std::string &source, const std::string &content,
          core::u64 fence = 0U, core::u64 authored_generation = 0U) noexcept;
  [[nodiscard]] bool refresh(const std::string &source) noexcept;
  [[nodiscard]] core::u64 publication_epoch() const noexcept;
  [[nodiscard]] ProbeCacheStats stats() const noexcept;
  void wait_for_test() const;
  void set_next_load_gate_for_test(
      std::shared_ptr<const std::function<void()>> gate);

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace mediaserver::render
